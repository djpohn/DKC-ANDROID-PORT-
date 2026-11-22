package com.dkc1.recomp;

import android.os.Handler;
import android.os.Looper;
import android.os.Process;

import org.libsdl.app.SDLActivity;

/** Runs the native game (libmain.so) through SDL2 in its own process (":game"). */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    /**
     * Called from native code on MENU INICIAL, after the save is flushed.
     * The game process is then stopped, so the next JOGAR starts from a clean
     * state. The home screen lives in the main process and is not affected.
     */
    public void goHome() {
        finish();
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override
            public void run() {
                Process.killProcess(Process.myPid());
            }
        }, 400);
    }
}
