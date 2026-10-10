package org.gp32emu.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.AtomicFile;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.locks.LockSupport;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final int PICK_BIOS = 1, PICK_GAME = 2;
    private static final int STATE_SLOTS = 3;
    private static final int ACCENT = Color.rgb(127, 209, 185), PANEL = Color.rgb(31, 37, 48);
    /* An old Activity can finish its save flush while the new one starts. */
    private static final Object CORE_OWNER = new Object();
    /** Held while drawing and while the surface is destroyed, so surfaceDestroyed
     *  returns only after the emulation thread has released the canvas. */
    private final Object surfaceLock = new Object();
    private final Object wake = new Object();
    final InputState input = new InputState();
    TouchPadView touchPad;
    private SurfaceView screen;
    private FrameLayout play;
    private Button menuButton;
    private LinearLayout home;
    private TextView biosStatus;
    private Button biosButton, continueButton;
    private TextView emptyText;
    private final List<File> games = new ArrayList<>();
    private ArrayAdapter<File> gameAdapter;
    private SharedPreferences prefs;
    private Thread worker;
    private final ExecutorService importer = Executors.newSingleThreadExecutor();
    private volatile boolean paused = true, stopped, surfaceReady, focusGranted, importing;
    private volatile boolean gameLoaded, uiHold = true, resetRequested;
    volatile boolean touchEnabled, integerScale;
    private volatile String runningGame;
    private String pendingGame;
    /** Guarded by wake; consumed on the emulation thread. */
    private StateRequest stateRequest;
    private int openDialogs; // UI thread only
    private volatile int pauseSequence;
    private AlertDialog menu;
    private AudioManager audioManager;

    private static final class StateRequest {
        final boolean save;
        final String game;
        final int slot;
        StateRequest(boolean save, String game, int slot) { this.save = save; this.game = game; this.slot = slot; }
    }
    private final AudioManager.OnAudioFocusChangeListener audioFocus = change -> {
        // A short notification may lower our volume; keep playing through it.
        focusGranted = change == AudioManager.AUDIOFOCUS_GAIN
                || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK;
        if (!focusGranted) clearInput();
        signal();
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        audioManager = (AudioManager)getSystemService(AUDIO_SERVICE);
        prefs = getPreferences(0);
        touchEnabled = prefs.getBoolean("touch", true);
        integerScale = prefs.getBoolean("integer", false);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        play = new FrameLayout(this);
        screen = new SurfaceView(this);
        screen.getHolder().addCallback(this);
        play.addView(screen, new FrameLayout.LayoutParams(-1, -1));
        touchPad = new TouchPadView(this, input);
        touchPad.setHaptics(prefs.getBoolean("haptics", true));
        play.addView(touchPad, new FrameLayout.LayoutParams(-1, -1));
        menuButton = new Button(this);
        menuButton.setText("\u2261");
        menuButton.setTextSize(22);
        menuButton.setTextColor(Color.WHITE);
        menuButton.setContentDescription(getString(R.string.menu));
        menuButton.setBackground(rounded(Color.argb(110, 26, 34, 46), 12));
        menuButton.setAlpha(0.8f);
        menuButton.setPadding(0, 0, 0, 0);
        menuButton.setOnClickListener(v -> showMenu());
        play.addView(menuButton, new FrameLayout.LayoutParams(dp(48), dp(48)));
        placeMenuButton(getResources().getConfiguration());
        play.setOnApplyWindowInsetsListener((view, insets) -> { padForInsets(view, insets); return insets; });
        root.addView(play, new FrameLayout.LayoutParams(-1, -1));
        home = buildHome();
        root.addView(home, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        setTouchEnabled(touchEnabled);
        directory("system"); directory("games"); directory("saves");
        refreshHome();
        worker = new Thread(this::emulate, "GP32 emulation");
        worker.start();
    }

    private LinearLayout buildHome() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(Color.rgb(20, 22, 27));
        page.setClickable(true); // The game surface below must not receive taps.
        page.setOnApplyWindowInsetsListener((view, insets) -> {
            padForInsets(view, insets);
            view.setPadding(view.getPaddingLeft() + dp(20), view.getPaddingTop() + dp(16),
                            view.getPaddingRight() + dp(20), view.getPaddingBottom() + dp(12));
            return insets;
        });
        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(28);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(Color.WHITE);
        page.addView(title);
        TextView tagline = new TextView(this);
        tagline.setText(R.string.tagline);
        tagline.setTextColor(Color.rgb(160, 170, 185));
        page.addView(tagline);

        LinearLayout bios = new LinearLayout(this);
        bios.setGravity(Gravity.CENTER_VERTICAL);
        bios.setPadding(dp(14), dp(8), dp(8), dp(8));
        bios.setBackground(rounded(PANEL, 12));
        biosStatus = new TextView(this);
        biosStatus.setTextColor(Color.rgb(220, 226, 235));
        biosStatus.setTextSize(14);
        bios.addView(biosStatus, new LinearLayout.LayoutParams(0, -2, 1));
        biosButton = new Button(this, null, android.R.attr.borderlessButtonStyle);
        biosButton.setTextColor(ACCENT);
        biosButton.setAllCaps(false);
        biosButton.setMinHeight(dp(48));
        biosButton.setOnClickListener(v -> choose(PICK_BIOS));
        bios.addView(biosButton, new LinearLayout.LayoutParams(-2, dp(48)));
        LinearLayout.LayoutParams biosParams = new LinearLayout.LayoutParams(-1, -2);
        biosParams.topMargin = dp(18);
        page.addView(bios, biosParams);

        continueButton = primaryButton();
        continueButton.setSingleLine(true);
        continueButton.setEllipsize(android.text.TextUtils.TruncateAt.END);
        continueButton.setPadding(dp(16), 0, dp(16), 0);
        continueButton.setOnClickListener(v -> {
            if (gameLoaded) showGame(); else if (!games.isEmpty()) launch(games.get(0));
        });
        LinearLayout.LayoutParams continueParams = new LinearLayout.LayoutParams(-1, dp(56));
        continueParams.topMargin = dp(16);
        page.addView(continueButton, continueParams);
        Button add = primaryButton();
        add.setTextColor(ACCENT);
        GradientDrawable outline = rounded(Color.TRANSPARENT, 14);
        outline.setStroke(dp(2), ACCENT);
        add.setBackground(outline);
        add.setText(R.string.add_game);
        add.setOnClickListener(v -> choose(PICK_GAME));
        LinearLayout.LayoutParams addParams = new LinearLayout.LayoutParams(-1, dp(56));
        addParams.topMargin = dp(10);
        page.addView(add, addParams);

        TextView header = new TextView(this);
        header.setText(R.string.library);
        header.setTextColor(Color.WHITE);
        header.setTextSize(16);
        header.setTypeface(Typeface.DEFAULT_BOLD);
        header.setPadding(0, dp(22), 0, dp(2));
        page.addView(header);
        TextView hint = new TextView(this);
        hint.setText(R.string.library_hint);
        hint.setTextColor(Color.rgb(140, 150, 165));
        hint.setTextSize(12);
        page.addView(hint);
        emptyText = new TextView(this);
        emptyText.setText(R.string.library_empty);
        emptyText.setTextColor(Color.rgb(170, 178, 190));
        emptyText.setPadding(0, dp(16), 0, 0);
        page.addView(emptyText);

        ListView list = new ListView(this);
        list.setDividerHeight(dp(6));
        list.setDivider(null);
        // Rows paint their own background, so draw the selector over them for D-pad/gamepad navigation.
        list.setSelector(rounded(Color.argb(70, 127, 209, 185), 10));
        list.setDrawSelectorOnTop(true);
        gameAdapter = new ArrayAdapter<File>(this, 0, games) {
            @Override public View getView(int position, View reuse, ViewGroup parent) {
                TextView row = reuse instanceof TextView ? (TextView)reuse : new TextView(MainActivity.this);
                File file = getItem(position);
                row.setText(title(file) + "\n" + String.format(Locale.ROOT, "%.1f MB", file.length() / 1048576.0));
                row.setTextColor(Color.WHITE);
                row.setTextSize(15);
                row.setMinHeight(dp(56));
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(dp(14), dp(8), dp(14), dp(8));
                row.setBackground(rounded(file.getAbsolutePath().equals(runningGame) ? Color.rgb(36, 60, 58) : PANEL, 10));
                return row;
            }
        };
        list.setAdapter(gameAdapter);
        list.setOnItemClickListener((parent, view, position, id) -> launch(games.get(position)));
        list.setOnItemLongClickListener((parent, view, position, id) -> { confirmRemove(games.get(position)); return true; });
        LinearLayout.LayoutParams listParams = new LinearLayout.LayoutParams(-1, 0, 1);
        listParams.topMargin = dp(10);
        page.addView(list, listParams);
        return page;
    }

    private Button primaryButton() {
        Button b = new Button(this, null, android.R.attr.borderlessButtonStyle);
        b.setAllCaps(false);
        b.setTextSize(16);
        b.setTextColor(Color.rgb(16, 32, 30));
        b.setBackground(rounded(ACCENT, 14));
        return b;
    }
    private GradientDrawable rounded(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        return d;
    }
    private void padForInsets(View view, WindowInsets insets) {
        int l = insets.getSystemWindowInsetLeft(), t = insets.getSystemWindowInsetTop();
        int r = insets.getSystemWindowInsetRight(), b = insets.getSystemWindowInsetBottom();
        if (Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
            DisplayCutout c = insets.getDisplayCutout();
            l = Math.max(l, c.getSafeInsetLeft()); t = Math.max(t, c.getSafeInsetTop());
            r = Math.max(r, c.getSafeInsetRight()); b = Math.max(b, c.getSafeInsetBottom());
        }
        view.setPadding(l, t, r, b);
    }
    private static String title(File file) {
        String name = file.getName();
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    private void refreshHome() {
        boolean haveBios = new File(directory("system"), "gp32166m.bin").isFile();
        biosStatus.setText(haveBios ? R.string.bios_ready : R.string.bios_missing);
        biosButton.setText(haveBios ? R.string.change_bios : R.string.choose_bios);
        games.clear();
        File[] folders = directory("games").listFiles();
        if (folders != null) for (File folder : folders) {
            File[] files = folder.listFiles();
            if (files != null) for (File f : files) if (f.isFile()) games.add(f);
        }
        games.sort((a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        gameAdapter.notifyDataSetChanged();
        emptyText.setVisibility(games.isEmpty() ? View.VISIBLE : View.GONE);
        File resume = gameLoaded && runningGame != null ? new File(runningGame) : games.isEmpty() ? null : games.get(0);
        continueButton.setVisibility(resume == null ? View.GONE : View.VISIBLE);
        if (resume != null) continueButton.setText(getString(R.string.resume_game, title(resume)));
    }
    private void showHome() {
        clearInput();
        uiHold = true; signal();
        refreshHome();
        home.setVisibility(View.VISIBLE);
        setFullscreen(false);
    }
    void showGame() {
        home.setVisibility(View.GONE);
        setFullscreen(true);
        uiHold = openDialogs > 0;
        signal();
    }
    @SuppressWarnings("deprecation")
    private void setFullscreen(boolean immersive) {
        getWindow().getDecorView().setSystemUiVisibility(immersive
            ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
              | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            : View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
    void launch(File game) {
        game.setLastModified(System.currentTimeMillis());
        if (!game.getAbsolutePath().equals(runningGame) || !gameLoaded) {
            gameLoaded = false;
            synchronized (wake) { pendingGame = game.getAbsolutePath(); wake.notifyAll(); }
            toast(getString(R.string.loading, title(game)));
        }
        showGame();
    }
    private void confirmRemove(File game) {
        if (game.getAbsolutePath().equals(runningGame)) { toast(getString(R.string.remove_running)); return; }
        new AlertDialog.Builder(this).setTitle(getString(R.string.remove_title, title(game)))
            .setMessage(R.string.remove_message).setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.remove, (d, w) -> {
                File folder = game.getParentFile();
                deleteTree(folder);
                deleteTree(new File(directory("saves"), folder.getName()));
                refreshHome();
            }).show();
    }
    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    private void showMenu() {
        if (menu != null && menu.isShowing()) return;
        List<String> labels = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();
        labels.add(getString(R.string.menu_resume)); actions.add(() -> {});
        labels.add(getString(R.string.menu_save_state)); actions.add(() -> showStateSlots(true));
        labels.add(getString(R.string.menu_load_state)); actions.add(() -> showStateSlots(false));
        labels.add(getString(R.string.menu_reset)); actions.add(() -> { resetRequested = true; });
        labels.add(getString(touchEnabled ? R.string.menu_touch_off : R.string.menu_touch_on));
        actions.add(() -> setTouchEnabled(!touchEnabled));
        labels.add(getString(touchPad.haptics() ? R.string.menu_haptics_off : R.string.menu_haptics_on));
        actions.add(() -> { touchPad.setHaptics(!touchPad.haptics()); prefs.edit().putBoolean("haptics", touchPad.haptics()).apply(); });
        labels.add(getString(integerScale ? R.string.menu_scale_fit : R.string.menu_scale_integer));
        actions.add(() -> { integerScale = !integerScale; prefs.edit().putBoolean("integer", integerScale).apply(); });
        labels.add(getString(R.string.menu_library)); actions.add(this::showHome);
        labels.add(getString(R.string.menu_about)); actions.add(this::showInfo);
        labels.add(getString(R.string.menu_quit)); actions.add(this::finish);
        menu = new AlertDialog.Builder(this).setTitle(runningGame == null ? getString(R.string.menu) : title(new File(runningGame)))
            .setItems(labels.toArray(new String[0]), (d, which) -> actions.get(which).run())
            .create();
        present(menu);
    }

    /** Shows a dialog and keeps the game paused until every dialog is closed.
     *  A gamepad's Mode/Menu button closes it again, since dialogs get the keys first. */
    private void present(AlertDialog dialog) {
        clearInput();
        ++openDialogs;
        uiHold = true; signal();
        dialog.setOnKeyListener((d, code, event) -> {
            if ((code == KeyEvent.KEYCODE_BUTTON_MODE || code == KeyEvent.KEYCODE_MENU) && event.getAction() == KeyEvent.ACTION_UP) {
                d.dismiss();
                return true;
            }
            return false;
        });
        dialog.setOnDismissListener(d -> {
            if (d == menu) menu = null;
            if (--openDialogs == 0 && home.getVisibility() != View.VISIBLE) { uiHold = false; setFullscreen(true); signal(); }
        });
        dialog.show();
    }

    private File stateFile(String game, int slot) {
        return new File(new File(getFilesDir(), "saves/" + new File(game).getParentFile().getName()), "state" + slot + ".gp32st");
    }
    private void showStateSlots(boolean save) {
        String game = runningGame;
        if (game == null || !gameLoaded) return;
        DateFormat format = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT);
        String[] labels = new String[STATE_SLOTS];
        boolean[] filled = new boolean[STATE_SLOTS];
        for (int i = 0; i < STATE_SLOTS; ++i) {
            File file = stateFile(game, i + 1);
            filled[i] = file.isFile() && file.length() > 0;
            labels[i] = getString(R.string.slot, i + 1) + "  \u00b7  "
                    + (filled[i] ? format.format(new Date(file.lastModified())) : getString(R.string.slot_empty));
        }
        // Loading needs a saved state, so empty slots are shown but disabled.
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, labels) {
            @Override public boolean isEnabled(int position) { return save || filled[position]; }
            @Override public View getView(int position, View reuse, ViewGroup parent) {
                View row = super.getView(position, reuse, parent);
                row.setAlpha(isEnabled(position) ? 1f : 0.4f);
                return row;
            }
        };
        present(new AlertDialog.Builder(this)
            .setTitle(save ? R.string.menu_save_state : R.string.menu_load_state)
            .setAdapter(adapter, (d, which) -> requestState(save, game, which + 1))
            .setNegativeButton(R.string.cancel, null).create());
    }
    private void requestState(boolean save, String game, int slot) {
        synchronized (wake) { stateRequest = new StateRequest(save, game, slot); wake.notifyAll(); }
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private File directory(String name) {
        File d = new File(getFilesDir(), name);
        if (!d.isDirectory() && !d.mkdirs()) throw new IllegalStateException("Cannot create " + name);
        return d;
    }
    void setTouchEnabled(boolean enabled) {
        touchEnabled = enabled;
        if (touchPad == null) return;
        touchPad.release();
        touchPad.setVisibility(enabled ? View.VISIBLE : View.GONE);
        prefs.edit().putBoolean("touch", enabled).apply();
    }
    private void clearInput() {
        input.clear();
        if (touchPad != null) touchPad.release();
    }
    private void signal() { synchronized (wake) { wake.notifyAll(); } }
    private void toast(String message) {
        runOnUiThread(() -> { if (!isDestroyed()) Toast.makeText(this, message, Toast.LENGTH_SHORT).show(); });
    }
    private void choose(int request) {
        if (importing) { toast(getString(R.string.import_busy)); return; }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, request);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request != PICK_BIOS && request != PICK_GAME) return;
        Uri uri = data.getData();
        importing = true;
        toast(getString(R.string.importing));
        importer.execute(() -> {
            File temporary = null;
            try {
                String name = "game.smc";
                try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (c != null && c.moveToFirst() && !c.isNull(0)) name = c.getString(0);
                }
                name = name.replaceAll("[\\\\/\\p{Cntrl}]", "_");
                String ext = name.toLowerCase(Locale.ROOT);
                if (request == PICK_GAME && !(ext.endsWith(".smc") || ext.endsWith(".fxe") || ext.endsWith(".fpk")))
                    throw new IllegalArgumentException(getString(R.string.import_type));
                temporary = File.createTempFile("gp32-import-", ".tmp", getCacheDir());
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                long total = 0, limit = request == PICK_BIOS ? 2L * 1024 * 1024 : 256L * 1024 * 1024;
                try (InputStream in = getContentResolver().openInputStream(uri);
                     FileOutputStream out = new FileOutputStream(temporary)) {
                    if (in == null) throw new IllegalArgumentException(getString(R.string.import_read));
                    byte[] buffer = new byte[65536];
                    int n;
                    while ((n = in.read(buffer)) != -1) {
                        total += n;
                        if (total > limit) throw new IllegalArgumentException(getString(R.string.import_large));
                        out.write(buffer, 0, n); digest.update(buffer, 0, n);
                    }
                    out.getFD().sync();
                }
                if (total == 0) throw new IllegalArgumentException(getString(R.string.import_empty));
                if (request == PICK_BIOS) {
                    AtomicFile target = new AtomicFile(new File(directory("system"), "gp32166m.bin"));
                    FileOutputStream out = null;
                    try (InputStream in = new FileInputStream(temporary)) {
                        out = target.startWrite();
                        byte[] buffer = new byte[65536]; int n;
                        while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
                        target.finishWrite(out); out = null;
                    } finally { if (out != null) target.failWrite(out); }
                    toast(getString(R.string.bios_imported));
                    runOnUiThread(this::refreshHome);
                } else {
                    StringBuilder id = new StringBuilder();
                    for (byte b : digest.digest()) id.append(String.format(Locale.ROOT, "%02x", b & 255));
                    File dest = new File(directory("games/" + id), name);
                    if (!dest.exists() && !temporary.renameTo(dest)) throw new IllegalStateException(getString(R.string.import_store));
                    runOnUiThread(() -> { refreshHome(); launch(dest); });
                }
            } catch (Exception e) { toast(getString(R.string.import_failed, e.getMessage())); }
            finally { if (temporary != null) temporary.delete(); importing = false; }
        });
    }

    private void emulate() {
        synchronized (CORE_OWNER) {
            while (!stopped) {
                runCore(); // Returns when stopped or after a fatal error.
                if (stopped) break;
                runOnUiThread(this::showHome);
                try {
                    synchronized (wake) { while (!stopped && pendingGame == null) wake.wait(); }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
    }
    private boolean held() { return paused || uiHold || !surfaceReady || !focusGranted || !gameLoaded; }
    private void runCore() {
        AudioTrack track = null;
        Bitmap image = null;
        boolean nativeOpened = false, suspended = true;
        long deadline = 0;
        int flushedPause = 0;
        try {
            image = Bitmap.createBitmap(320, 240, Bitmap.Config.ARGB_8888);
            short[] pcm = new short[8192];
            Paint paint = new Paint(); // Nearest-neighbour pixels, no per-frame objects.
            Rect dest = new Rect();
            int minimum = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) throw new IllegalStateException("44.1 kHz stereo audio is unavailable.");
            track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(44100)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                // 80 ms floor: enough margin for one late frame without audible lag.
                .setBufferSizeInBytes(Math.max(minimum, 3528 * 4))
                .setTransferMode(AudioTrack.MODE_STREAM).build();
            if (track.getState() != AudioTrack.STATE_INITIALIZED) throw new IllegalStateException("Audio output initialization failed.");
            while (!stopped) {
                int requestedPause = pauseSequence;
                if (nativeOpened && requestedPause != flushedPause) {
                    if (!NativeCore.flush()) toast(getString(R.string.save_failed));
                    flushedPause = requestedPause;
                }
                String game;
                synchronized (wake) { game = pendingGame; pendingGame = null; }
                if (game != null) {
                    track.pause(); track.flush(); suspended = true;
                    gameLoaded = false;
                    File source = new File(game);
                    File saves = directory("saves/" + source.getParentFile().getName());
                    nativeOpened = true;
                    String error = NativeCore.open(directory("system").getAbsolutePath(), saves.getAbsolutePath(), game);
                    runningGame = error == null ? game : null;
                    gameLoaded = error == null;
                    if (error != null) { toast(error); runOnUiThread(this::showHome); }
                    deadline = 0;
                }
                if (gameLoaded && resetRequested) {
                    // Drop sound queued before the reset so it is not heard afterwards.
                    resetRequested = false;
                    track.pause(); track.flush(); suspended = true;
                    NativeCore.reset();
                    deadline = 0;
                }
                StateRequest request;
                synchronized (wake) { request = stateRequest; stateRequest = null; }
                if (request != null && gameLoaded && request.game.equals(runningGame)) {
                    // Runs here so the core is only ever touched by this thread.
                    String path = stateFile(request.game, request.slot).getAbsolutePath();
                    String error = request.save ? NativeCore.saveState(path) : NativeCore.loadState(path);
                    if (error != null) toast(getString(request.save ? R.string.state_save_failed : R.string.state_load_failed, error));
                    else toast(getString(request.save ? R.string.state_saved : R.string.state_loaded, request.slot));
                    if (!request.save && error == null) {
                        // Drop sound queued before the load and restart the frame clock.
                        track.pause(); track.flush(); suspended = true;
                        deadline = 0;
                    }
                }
                if (held()) {
                    if (!suspended) {
                        track.pause(); track.flush();
                        if (!NativeCore.flush()) toast(getString(R.string.save_failed));
                        suspended = true;
                    }
                    synchronized (wake) {
                        if (!stopped && pendingGame == null && !resetRequested && stateRequest == null && held()) wake.wait();
                    }
                    deadline = 0;
                    continue;
                }
                if (suspended) { track.play(); suspended = false; }
                int count = NativeCore.frame(input.mask(), image, pcm);
                if (count < 0) throw new IllegalStateException("Frame delivery failed.");
                int written = 0;
                while (written < count && !stopped && !paused) {
                    int n = track.write(pcm, written, count - written, AudioTrack.WRITE_BLOCKING);
                    if (n <= 0) throw new IllegalStateException("Audio output stopped (" + n + ").");
                    written += n;
                }
                drawFrame(image, paint, dest);
                String message = NativeCore.message();
                if (message != null) toast(message);
                // Keep guest speed at 60 frontend frames/s even during silence.
                long now = System.nanoTime();
                if (deadline == 0 || now - deadline > 100_000_000L) deadline = now;
                deadline += 16_666_667L;
                long remaining = deadline - System.nanoTime();
                if (remaining > 0) LockSupport.parkNanos(remaining);
            }
        } catch (Exception | LinkageError e) {
            toast(getString(R.string.stopped, e.getMessage()));
        } finally {
            gameLoaded = false;
            if (track != null) { track.pause(); track.flush(); track.release(); }
            if (nativeOpened) { NativeCore.flush(); NativeCore.close(); }
            if (image != null) image.recycle();
        }
    }
    private void drawFrame(Bitmap image, Paint paint, Rect dest) {
        synchronized (surfaceLock) {
            if (!surfaceReady) return;
            Canvas canvas = null;
            try {
                canvas = screen.getHolder().lockCanvas();
                if (canvas == null) return;
                canvas.drawColor(Color.BLACK);
                int width = canvas.getWidth(), height = canvas.getHeight();
                int top = 0, bottom = height;
                if (height > width) {
                    top = dp(60); // Menu button row in portrait.
                    // Before the touch pad's first layout its edge is still unknown.
                    if (touchEnabled) bottom = Math.max(top + 1,
                            (int)Math.min(height, touchPad.controlsTop()) - dp(8));
                }
                int areaW = width, areaH = Math.max(1, bottom - top);
                int w, h;
                if (integerScale && Math.min(areaW / 320, areaH / 240) >= 1) {
                    int scale = Math.min(areaW / 320, areaH / 240);
                    w = 320 * scale; h = 240 * scale;
                } else {
                    w = Math.min(areaW, areaH * 4 / 3); h = w * 3 / 4;
                }
                int x = (width - w) / 2, y = top + (areaH - h) / 2;
                dest.set(x, y, x + w, y + h);
                canvas.drawBitmap(image, null, dest, paint);
            } finally { if (canvas != null) screen.getHolder().unlockCanvasAndPost(canvas); }
        }
    }
    private void showInfo() {
        present(new AlertDialog.Builder(this).setTitle(R.string.about).setMessage(R.string.about_text)
            .setPositiveButton(R.string.ok, null).setNeutralButton(R.string.licenses, (d, w) -> showLicenses())
            .create());
    }
    private void showLicenses() {
        StringBuilder text = new StringBuilder();
        try {
            readLicenses("licenses", text);
        } catch (Exception e) { text.append(e.getMessage()); }
        present(new AlertDialog.Builder(this).setTitle(R.string.licenses).setMessage(text).setPositiveButton(R.string.ok, null).create());
    }
    private void readLicenses(String path, StringBuilder text) throws java.io.IOException {
        String[] names = getAssets().list(path);
        if (names != null && names.length > 0) {
            Arrays.sort(names);
            for (String name : names) readLicenses(path + "/" + name, text);
        } else {
            text.append(path).append("\n\n");
            try (InputStream in = getAssets().open(path)) {
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                byte[] b = new byte[4096]; int n;
                while ((n = in.read(b)) != -1) bytes.write(b, 0, n);
                text.append(new String(bytes.toByteArray(), StandardCharsets.UTF_8)).append("\n\n");
            }
        }
    }
    private boolean inGame() { return home.getVisibility() != View.VISIBLE && gameLoaded; }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_BUTTON_MODE || code == KeyEvent.KEYCODE_MENU) {
            if (event.getAction() == KeyEvent.ACTION_UP && inGame()) showMenu();
            return true;
        }
        if (inGame() && !uiHold && input.key(event)) {
            // Start+Select together on a gamepad or keyboard opens the menu.
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0 && input.menuChord()) showMenu();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (inGame() && !uiHold && input.motion(event)) return true;
        return super.dispatchGenericMotionEvent(event);
    }
    @Override protected void onResume() {
        super.onResume(); paused = false;
        focusGranted = audioManager.requestAudioFocus(audioFocus, AudioManager.STREAM_MUSIC,
                AudioManager.AUDIOFOCUS_GAIN) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        if (home.getVisibility() != View.VISIBLE) setFullscreen(true);
        signal();
    }
    @Override protected void onPause() {
        paused = true; ++pauseSequence; clearInput(); signal();
        audioManager.abandonAudioFocus(audioFocus); focusGranted = false;
        super.onPause();
    }
    @Override protected void onDestroy() {
        stopped = true; clearInput(); signal(); importer.shutdown();
        super.onDestroy();
    }
    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        clearInput();
        placeMenuButton(config);
    }
    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus) clearInput();
        else if (home.getVisibility() != View.VISIBLE) setFullscreen(true);
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { surfaceReady = true; signal(); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { clearInput(); signal(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        synchronized (surfaceLock) { surfaceReady = false; }
        clearInput(); signal();
    }
    @Override public void onBackPressed() {
        if (home.getVisibility() != View.VISIBLE) showMenu();
        else if (gameLoaded) showGame();
        else finish();
    }
    /** Portrait: top-right above the picture. Landscape: top centre, clear of L/R. */
    private void placeMenuButton(Configuration config) {
        FrameLayout.LayoutParams p = (FrameLayout.LayoutParams)menuButton.getLayoutParams();
        p.gravity = Gravity.TOP | (config.orientation == Configuration.ORIENTATION_LANDSCAPE
                ? Gravity.CENTER_HORIZONTAL : Gravity.END);
        // Over the picture in landscape, so keep it faint there.
        menuButton.setAlpha(config.orientation == Configuration.ORIENTATION_LANDSCAPE ? 0.45f : 0.8f);
        p.setMargins(dp(6), dp(6), dp(6), dp(6));
        menuButton.setLayoutParams(p);
    }
}
