package com.dkc1.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Home screen. Top: language and options. Centre: title, status and the main
 * buttons. The background is drawn in code; no game art is used.
 */
public class RomPickerActivity extends Activity {
    private static final int REQUEST_ROM = 1;
    private static final int GOLD = Color.rgb(255, 210, 63);

    private File romFile;
    private String lang;
    private TextView status;
    private Button play;
    private Button changeRom;
    private Button languageButton;
    private Button optionsButton;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        romFile = new File(getFilesDir(), "game.sfc");
        lang = getSharedPreferences("dkc1", MODE_PRIVATE).getString("lang", "pt");
        askForAllFilesAccess();

        FrameLayout root = new FrameLayout(this);
        ImageView backdrop = new ImageView(this);
        backdrop.setImageResource(R.drawable.home_bg);
        backdrop.setScaleType(ImageView.ScaleType.CENTER_CROP);
        root.addView(backdrop, match());

        // Top bar: language on the left, options on the right.
        FrameLayout top = new FrameLayout(this);
        languageButton = pill(false);
        languageButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                PopupMenu menu = new PopupMenu(RomPickerActivity.this, v);
                menu.getMenu().add(0, 0, 0, "Portugues");
                menu.getMenu().add(0, 1, 1, "English");
                menu.getMenu().add(0, 2, 2, "Espanol");
                menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
                    @Override
                    public boolean onMenuItemClick(android.view.MenuItem item) {
                        String[] codes = {"pt", "en", "es"};
                        getSharedPreferences("dkc1", MODE_PRIVATE).edit()
                                .putString("lang", codes[item.getItemId()]).apply();
                        recreate();
                        return true;
                    }
                });
                menu.show();
            }
        });
        FrameLayout.LayoutParams leftLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.START | Gravity.TOP);
        leftLp.setMargins(dp(20), dp(20), 0, 0);
        top.addView(languageButton, leftLp);

        optionsButton = pill(false);
        optionsButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                PopupMenu menu = new PopupMenu(RomPickerActivity.this, v);
                menu.getMenu().add(0, 0, 0, labelCredits());
                menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
                    @Override
                    public boolean onMenuItemClick(android.view.MenuItem item) {
                        showCredits();
                        return true;
                    }
                });
                menu.show();
            }
        });
        FrameLayout.LayoutParams rightLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.END | Gravity.TOP);
        rightLp.setMargins(0, dp(20), dp(20), 0);
        top.addView(optionsButton, rightLp);
        root.addView(top, match());

        // Centre column: title, status, main buttons.
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_HORIZONTAL);
        column.setPadding(dp(40), 0, dp(40), 0);

        TextView title = text(28, GOLD, true);
        title.setText("DONKEY KONG");
        column.addView(title);
        TextView title2 = text(20, Color.WHITE, true);
        title2.setText("COUNTRY");
        column.addView(title2);

        TextView subtitle = text(13, Color.rgb(220, 240, 200), false);
        subtitle.setText("Port nao oficial para Android - V1.0");
        subtitle.setPadding(0, dp(6), 0, dp(18));
        column.addView(subtitle);

        status = text(15, Color.WHITE, false);
        status.setPadding(0, 0, 0, dp(18));
        column.addView(status);

        play = mainButton();
        play.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startGame();
            }
        });
        column.addView(play, buttonLp(dp(56)));

        changeRom = pill(true);
        changeRom.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, REQUEST_ROM);
            }
        });
        column.addView(changeRom, buttonLp(dp(48)));

        root.addView(column, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));

        // Footer: graphics API on the left, credit on the right.
        FrameLayout footer = new FrameLayout(this);
        TextView api = text(12, Color.rgb(220, 240, 200), false);
        api.setText("Graficos: OpenGL ES 2");
        FrameLayout.LayoutParams apiLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.START | Gravity.BOTTOM);
        apiLp.setMargins(dp(20), 0, 0, dp(16));
        footer.addView(api, apiLp);
        TextView credit = text(14, Color.WHITE, true);
        credit.setText("MR.HIPEE");
        FrameLayout.LayoutParams creditLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.END | Gravity.BOTTOM);
        creditLp.setMargins(0, 0, dp(20), dp(16));
        footer.addView(credit, creditLp);
        root.addView(footer, match());

        setContentView(root);
        applyLanguage();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyLanguage();
    }

    private void applyLanguage() {
        boolean hasRom = romFile.length() > 0;
        languageButton.setText(lang.toUpperCase() + "  ▾");
        optionsButton.setText(s("Opcoes", "Options", "Opciones") + "  ▾");
        play.setText(s("JOGAR", "PLAY", "JUGAR"));
        play.setEnabled(hasRom);
        play.setAlpha(hasRom ? 1f : 0.45f);
        changeRom.setText(hasRom
                ? s("Trocar arquivo da ROM", "Change ROM file", "Cambiar archivo de ROM")
                : s("Escolher ROM", "Choose ROM", "Elegir ROM"));
        status.setText(hasRom
                ? "✓  " + s("Pronto pra jogar", "Ready to play", "Listo para jugar")
                : s("Escolha sua ROM de Donkey Kong Country (USA 1.0, .sfc ou .smc).",
                    "Choose your Donkey Kong Country ROM (USA 1.0, .sfc or .smc).",
                    "Elige tu ROM de Donkey Kong Country (USA 1.0, .sfc o .smc)."));
    }

    private String labelCredits() {
        return s("Creditos", "Credits", "Creditos");
    }

    private void showCredits() {
        new AlertDialog.Builder(this)
                .setTitle(labelCredits())
                .setMessage("MR.HIPEE")
                .setPositiveButton("OK", null)
                .show();
    }

    private String s(String pt, String en, String es) {
        if ("en".equals(lang)) return en;
        if ("es".equals(lang)) return es;
        return pt;
    }

    /** Opens the game. */
    private void startGame() {
        if (romFile.length() <= 0) {
            Toast.makeText(this, s("Escolha a ROM primeiro", "Choose the ROM first",
                    "Elige la ROM primero"), Toast.LENGTH_LONG).show();
            return;
        }
        startActivity(new Intent(this, GameActivity.class));
    }

    /** Saves go to /storage/emulated/0/DKC1Recomp, which needs "all files access". */
    private void askForAllFilesAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        if (Environment.isExternalStorageManager()) return;
        SharedPreferences prefs = getSharedPreferences("dkc1", MODE_PRIVATE);
        if (prefs.getBoolean("asked_all_files", false)) return;
        prefs.edit().putBoolean("asked_all_files", true).apply();
        Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + getPackageName()));
        startActivity(intent);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ROM || resultCode != RESULT_OK || data == null
                || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        changeRom.setEnabled(false);
        status.setText(s("Copiando a ROM...", "Copying the ROM...", "Copiando la ROM..."));
        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean ok = copyRom(uri);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        changeRom.setEnabled(true);
                        applyLanguage();
                        if (!ok) {
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

    // ---- views -------------------------------------------------------------

    private int dp(int value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics()));
    }

    private static FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private TextView text(int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER);
        if (bold) t.setTypeface(null, android.graphics.Typeface.BOLD);
        return t;
    }

    private LinearLayout.LayoutParams buttonLp(int height) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, height);
        lp.setMargins(0, dp(8), 0, dp(8));
        return lp;
    }

    /** White pill, the main action (JOGAR). */
    private Button mainButton() {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setTextSize(18);
        b.setTypeface(null, android.graphics.Typeface.BOLD);
        b.setTextColor(Color.rgb(40, 24, 8));
        GradientDrawable body = new GradientDrawable();
        body.setShape(GradientDrawable.RECTANGLE);
        body.setCornerRadius(dp(40));
        body.setColor(Color.WHITE);
        b.setBackground(body);
        return b;
    }

    /** Translucent pill with a light border: a secondary action. */
    private Button pill(boolean wide) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setTextSize(15);
        b.setTextColor(Color.WHITE);
        GradientDrawable body = new GradientDrawable();
        body.setShape(GradientDrawable.RECTANGLE);
        body.setCornerRadius(dp(40));
        body.setColor(Color.argb(wide ? 60 : 80, 255, 255, 255));
        body.setStroke(dp(2), Color.argb(170, 255, 255, 255));
        b.setBackground(body);
        return b;
    }
}
