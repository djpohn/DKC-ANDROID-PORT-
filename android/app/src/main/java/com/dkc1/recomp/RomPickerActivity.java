package com.dkc1.recomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Launcher activity. If no ROM has been imported yet it asks the user to pick
 * one with the system file picker and copies it into the app's private files
 * directory as game.sfc (the native code loads it from there). The ROM is
 * never bundled in the APK. Once a ROM exists, it starts the game directly.
 */
public class RomPickerActivity extends Activity {
    private static final int REQUEST_ROM = 1;

    private File romFile;
    private TextView status;
    private Button pick;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        romFile = new File(getFilesDir(), "game.sfc");
        if (romFile.length() > 0) {
            startGame();
            return;
        }

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(48, 48, 48, 48);

        TextView title = new TextView(this);
        title.setText("DKC1Recomp");
        title.setTextSize(26);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        status = new TextView(this);
        status.setText("Escolha o arquivo da sua ROM de Donkey Kong Country "
                + "(versao USA 1.0, .sfc ou .smc). Ela fica so no seu celular.");
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, 32, 0, 32);
        root.addView(status);

        pick = new Button(this);
        pick.setText("Escolher ROM");
        pick.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, REQUEST_ROM);
            }
        });
        root.addView(pick);
        setContentView(root);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ROM || resultCode != RESULT_OK || data == null
                || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        pick.setEnabled(false);
        status.setText("Copiando a ROM...");
        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean ok = copyRom(uri);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (ok) {
                            startGame();
                        } else {
                            pick.setEnabled(true);
                            status.setText("Nao consegui copiar o arquivo. Tente de novo.");
                            Toast.makeText(RomPickerActivity.this, "Falha ao copiar a ROM",
                                    Toast.LENGTH_LONG).show();
                        }
                    }
                });
            }
        }).start();
    }

    /** Copy through a temporary file so a half-copied ROM is never used. */
    private boolean copyRom(Uri uri) {
        File temporary = new File(getFilesDir(), "game.sfc.tmp");
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(temporary)) {
            if (in == null) return false;
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        } catch (IOException e) {
            temporary.delete();
            return false;
        }
        if (romFile.exists() && !romFile.delete()) return false;
        return temporary.renameTo(romFile);
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }
}
