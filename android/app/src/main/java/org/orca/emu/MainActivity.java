package org.orca.emu;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/** Start screen: pick a game, the Wii common key file, the region of a new NAND, JIT on/off. */
public final class MainActivity extends Activity {
    private static final int PICK_GAME = 1;
    private static final int PICK_KEY = 2;
    private static final int PICK_DSP_ROM = 3;
    private static final int PICK_DSP_COEF = 4;
    static final String PREFS = "orca";

    private TextView keyState;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 48, 48, 48);

        TextView title = new TextView(this);
        title.setText("Orca Core 0.1 — GameCube / Wii");
        title.setTextSize(22);
        root.addView(title);

        Button game = new Button(this);
        game.setText("Choose a game (ISO/GCM)");
        game.setOnClickListener(v -> pick(PICK_GAME));
        root.addView(game);

        Button key = new Button(this);
        key.setText("Choose the Wii common key file (Wii games)");
        key.setOnClickListener(v -> pick(PICK_KEY));
        root.addView(key);
        keyState = new TextView(this);
        root.addView(keyState);
        updateKeyState();

        Button rom = new Button(this);
        rom.setText("Optional: DSP ROM dump (dsp_rom.bin, 8 KiB)");
        rom.setOnClickListener(v -> pick(PICK_DSP_ROM));
        root.addView(rom);
        Button coefButton = new Button(this);
        coefButton.setText("Optional: DSP COEF dump (dsp_coef.bin, 4 KiB)");
        coefButton.setOnClickListener(v -> pick(PICK_DSP_COEF));
        root.addView(coefButton);

        TextView regionLabel = new TextView(this);
        regionLabel.setText("Region of a new Wii NAND:");
        root.addView(regionLabel);
        Spinner region = new Spinner(this);
        region.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item,
                new String[] {"USA", "Europe", "Japan", "Korea"}));
        region.setSelection(prefs.getInt("region", 0));
        region.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(android.widget.AdapterView<?> p, android.view.View v, int pos, long id) {
                prefs.edit().putInt("region", pos).apply();
            }

            @Override
            public void onNothingSelected(android.widget.AdapterView<?> p) {}
        });
        root.addView(region);

        CheckBox jit = new CheckBox(this);
        jit.setText("JIT (PowerPC to ARM64)");
        jit.setChecked(prefs.getBoolean("jit", true));
        jit.setOnCheckedChangeListener((b, on) -> prefs.edit().putBoolean("jit", on).apply());
        root.addView(jit);

        CheckBox gles = new CheckBox(this);
        gles.setText("GPU renderer (OpenGL ES 3.2; software renderer when unavailable)");
        gles.setChecked(prefs.getBoolean("gles", true));
        gles.setOnCheckedChangeListener((b, on) -> prefs.edit().putBoolean("gles", on).apply());
        root.addView(gles);

        setContentView(root, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private void pick(int what) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, what);
    }

    static File keyFile(Activity a) {
        return new File(a.getFilesDir(), "wii_common_key.bin");
    }

    static File dspRom(Activity a) {
        return new File(a.getFilesDir(), "dsp_rom.bin");
    }

    static File dspCoef(Activity a) {
        return new File(a.getFilesDir(), "dsp_coef.bin");
    }

    private void updateKeyState() {
        keyState.setText((keyFile(this).exists() ? "Wii key: present" : "Wii key: not set") + " — DSP: "
                + (dspRom(this).exists() && dspCoef(this).exists() ? "ROM dumps (low-level DSP)" : "high-level DSP"));
    }

    private void copy(Uri uri, File to) {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[4096];
            int n;
            while (in != null && (n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (Exception e) {
            Toast.makeText(this, "Cannot copy the file: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
        updateKeyState();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (request == PICK_KEY) { copy(uri, keyFile(this)); return; }
        if (request == PICK_DSP_ROM) { copy(uri, dspRom(this)); return; }
        if (request == PICK_DSP_COEF) { copy(uri, dspCoef(this)); return; }
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // the permission lasts for this session only
        }
        Intent run = new Intent(this, EmulationActivity.class);
        run.setData(uri);
        startActivity(run);
    }
}
