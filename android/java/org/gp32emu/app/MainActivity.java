package org.gp32emu.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.AtomicFile;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.locks.LockSupport;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final int PICK_BIOS = 1, PICK_GAME = 2;
    /* An old Activity can finish its save flush while the new one starts. */
    private static final Object CORE_OWNER = new Object();
    private final Object wake = new Object();
    final InputState input = new InputState();
    TouchPadView touchPad;
    Switch touchSwitch;
    private SurfaceView screen;
    private TextView status;
    private Thread worker;
    private final ExecutorService importer = Executors.newSingleThreadExecutor();
    private volatile boolean paused = true, stopped, surfaceReady, focusGranted, importing;
    private volatile boolean gameLoaded;
    private String pendingGame;
    private volatile int pauseSequence;
    private AudioManager audioManager;
    private final AudioManager.OnAudioFocusChangeListener audioFocus = change -> {
        focusGranted = change == AudioManager.AUDIOFOCUS_GAIN;
        if (!focusGranted) clearInput();
        signal();
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        audioManager = (AudioManager)getSystemService(AUDIO_SERVICE);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setBackgroundColor(Color.rgb(20, 22, 27));
        // Respect navigation/status bars, including Android 15 edge-to-edge.
        layout.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                            insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets.consumeSystemWindowInsets();
        });
        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setGravity(android.view.Gravity.CENTER_VERTICAL);
        addButton(toolbar, "BIOS", () -> choose(PICK_BIOS));
        addButton(toolbar, "Game", () -> choose(PICK_GAME));
        touchSwitch = new Switch(this);
        touchSwitch.setText("Touch");
        touchSwitch.setContentDescription("Show touch controls");
        touchSwitch.setMinHeight(dp(48));
        touchSwitch.setChecked(getPreferences(0).getBoolean("touch", true));
        toolbar.addView(touchSwitch, new LinearLayout.LayoutParams(0, -2, 1));
        addButton(toolbar, "Info", this::showInfo);
        layout.addView(toolbar);
        status = new TextView(this);
        status.setTextSize(12);
        status.setPadding(dp(8), dp(2), dp(8), dp(2));
        status.setText("Select your BIOS, then an SMC, FXE or FPK game (extract ZIPs first).");
        layout.addView(status);
        FrameLayout play = new FrameLayout(this);
        screen = new SurfaceView(this);
        screen.getHolder().addCallback(this);
        play.addView(screen, new FrameLayout.LayoutParams(-1, -1));
        touchPad = new TouchPadView(this, input);
        play.addView(touchPad, new FrameLayout.LayoutParams(-1, -1));
        setTouchEnabled(touchSwitch.isChecked());
        touchSwitch.setOnCheckedChangeListener((button, enabled) -> setTouchEnabled(enabled));
        layout.addView(play, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(layout);
        directory("system"); directory("games"); directory("saves");
        String previous = getPreferences(0).getString("game", null);
        if (previous != null && new File(previous).isFile()) pendingGame = previous;
        worker = new Thread(this::emulate, "GP32 emulation");
        worker.start();
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private File directory(String name) {
        File d = new File(getFilesDir(), name);
        if (!d.isDirectory() && !d.mkdirs()) throw new IllegalStateException("Cannot create " + name);
        return d;
    }
    private void addButton(LinearLayout bar, String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label); b.setMinWidth(dp(56)); b.setMinimumWidth(dp(56));
        b.setPadding(dp(6), 0, dp(6), 0);
        b.setOnClickListener(v -> { clearInput(); action.run(); });
        bar.addView(b, new LinearLayout.LayoutParams(dp(62), dp(48)));
    }
    void setTouchEnabled(boolean enabled) {
        if (touchPad == null) return;
        touchPad.release();
        touchPad.setVisibility(enabled ? View.VISIBLE : View.GONE);
        getPreferences(0).edit().putBoolean("touch", enabled).apply();
    }
    private void clearInput() {
        input.clear();
        if (touchPad != null) touchPad.release();
    }
    private void signal() { synchronized (wake) { wake.notifyAll(); } }
    private void tell(String message) {
        runOnUiThread(() -> { if (!isDestroyed()) status.setText(message); });
    }
    private void choose(int request) {
        if (importing) { tell("A file is still being imported."); return; }
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
        tell("Importing…");
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
                    throw new IllegalArgumentException("Choose an SMC, FXE or FPK file. Extract ZIP archives first.");
                temporary = File.createTempFile("gp32-import-", ".tmp", getCacheDir());
                MessageDigest digest = MessageDigest.getInstance("SHA-256");
                long total = 0, limit = request == PICK_BIOS ? 2L * 1024 * 1024 : 256L * 1024 * 1024;
                try (InputStream in = getContentResolver().openInputStream(uri);
                     FileOutputStream out = new FileOutputStream(temporary)) {
                    if (in == null) throw new IllegalArgumentException("Cannot read the selected file.");
                    byte[] buffer = new byte[65536];
                    int n;
                    while ((n = in.read(buffer)) != -1) {
                        total += n;
                        if (total > limit) throw new IllegalArgumentException("The selected file is too large.");
                        out.write(buffer, 0, n); digest.update(buffer, 0, n);
                    }
                    out.getFD().sync();
                }
                if (total == 0) throw new IllegalArgumentException("The selected file is empty.");
                if (request == PICK_BIOS) {
                    AtomicFile target = new AtomicFile(new File(directory("system"), "gp32166m.bin"));
                    FileOutputStream out = null;
                    try (InputStream in = new FileInputStream(temporary)) {
                        out = target.startWrite();
                        byte[] buffer = new byte[65536]; int n;
                        while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
                        target.finishWrite(out); out = null;
                    } finally { if (out != null) target.failWrite(out); }
                    tell("BIOS imported. Open a game to use it.");
                } else {
                    StringBuilder id = new StringBuilder();
                    for (byte b : digest.digest()) id.append(String.format(Locale.ROOT, "%02x", b & 255));
                    File dest = new File(directory("games/" + id), name);
                    if (!dest.exists() && !temporary.renameTo(dest)) throw new IllegalStateException("Cannot store the game.");
                    synchronized (wake) { pendingGame = dest.getAbsolutePath(); wake.notifyAll(); }
                    getPreferences(0).edit().putString("game", dest.getAbsolutePath()).apply();
                }
            } catch (Exception e) { tell("Import failed: " + e.getMessage()); }
            finally { if (temporary != null) temporary.delete(); importing = false; }
        });
    }

    private void emulate() {
        synchronized (CORE_OWNER) { runCore(); }
    }
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
                .setBufferSizeInBytes(Math.max(minimum, 2205 * 4))
                .setTransferMode(AudioTrack.MODE_STREAM).build();
            if (track.getState() != AudioTrack.STATE_INITIALIZED) throw new IllegalStateException("Audio output initialization failed.");
            while (!stopped) {
                int requestedPause = pauseSequence;
                if (nativeOpened && requestedPause != flushedPause) {
                    if (!NativeCore.flush()) tell("Could not save progress. Check available storage.");
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
                    gameLoaded = error == null;
                    tell(error == null ? source.getName() : error);
                    deadline = 0;
                }
                if (paused || !surfaceReady || !focusGranted || !gameLoaded) {
                    if (!suspended) {
                        track.pause(); track.flush();
                        if (!NativeCore.flush()) tell("Could not save progress. Check available storage.");
                        suspended = true;
                    }
                    synchronized (wake) {
                        if (!stopped && pendingGame == null && (paused || !surfaceReady || !focusGranted || !gameLoaded)) wake.wait();
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
                if (message != null) tell(message);
                // Keep guest speed at 60 frontend frames/s even during silence.
                long now = System.nanoTime();
                if (deadline == 0 || now - deadline > 100_000_000L) deadline = now;
                deadline += 16_666_667L;
                long remaining = deadline - System.nanoTime();
                if (remaining > 0) LockSupport.parkNanos(remaining);
            }
        } catch (Exception | LinkageError e) {
            tell("Emulation stopped: " + e.getMessage());
        } finally {
            gameLoaded = false;
            if (track != null) { track.pause(); track.flush(); track.release(); }
            if (nativeOpened) { NativeCore.flush(); NativeCore.close(); }
            if (image != null) image.recycle();
        }
    }
    private void drawFrame(Bitmap image, Paint paint, Rect dest) {
        if (!surfaceReady) return;
        Canvas canvas = null;
        try {
            canvas = screen.getHolder().lockCanvas();
            if (canvas == null) return;
            canvas.drawColor(Color.BLACK);
            int width = canvas.getWidth(), height = canvas.getHeight();
            int available = height;
            if (height > width && getPreferences(0).getBoolean("touch", true)) available = Math.max(1, height - dp(235));
            int w = Math.min(width, available * 4 / 3), h = w * 3 / 4;
            int x = (width - w) / 2, y = (available - h) / 2;
            dest.set(x, y, x + w, y + h);
            canvas.drawBitmap(image, null, dest, paint);
        } finally { if (canvas != null) screen.getHolder().unlockCanvasAndPost(canvas); }
    }
    private void showInfo() {
        paused = true; clearInput(); signal();
        String text = "GP32emu 1.0.0\n\nBIOS: select your GP32 BIOS. Game: select an extracted SMC, FXE or FPK. The last game reopens next time.\n\nTouch switches the on-screen controls on/off. Bluetooth/USB gamepads work with Touch off. Keyboard: arrows, Z/X = A/B, A/S = L/R, Enter = Start, Shift = Select.\n\nGame files are copied into app storage; originals are unchanged. In-game saves are automatic. Back closes the game and exits. Uninstalling deletes imported files and saves.\n\nBased on gameblabla/gp32emu. Source: github.com/gajae1/gp32emu";
        new AlertDialog.Builder(this).setTitle("GP32emu").setMessage(text)
            .setPositiveButton("OK", null).setNeutralButton("Licenses", (d, w) -> showLicenses())
            .setOnDismissListener(d -> { paused = false; signal(); }).show();
    }
    private void showLicenses() {
        StringBuilder text = new StringBuilder();
        try {
            readLicenses("licenses", text);
        } catch (Exception e) { text.append(e.getMessage()); }
        new AlertDialog.Builder(this).setTitle("Licenses").setMessage(text).setPositiveButton("OK", null).show();
    }
    private void readLicenses(String path, StringBuilder text) throws java.io.IOException {
        String[] names = getAssets().list(path);
        if (names != null && names.length > 0) {
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
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (gameLoaded && input.key(event)) return true;
        return super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (gameLoaded && input.motion(event)) return true;
        return super.dispatchGenericMotionEvent(event);
    }
    @Override protected void onResume() {
        super.onResume(); paused = false;
        focusGranted = audioManager.requestAudioFocus(audioFocus, AudioManager.STREAM_MUSIC,
                AudioManager.AUDIOFOCUS_GAIN) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
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
    @Override public void onConfigurationChanged(Configuration config) { super.onConfigurationChanged(config); clearInput(); }
    @Override public void onWindowFocusChanged(boolean hasFocus) { super.onWindowFocusChanged(hasFocus); if (!hasFocus) clearInput(); }
    @Override public void surfaceCreated(SurfaceHolder holder) { surfaceReady = true; signal(); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { clearInput(); signal(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { surfaceReady = false; clearInput(); signal(); }
    @Override public void onBackPressed() { clearInput(); finish(); }
}
