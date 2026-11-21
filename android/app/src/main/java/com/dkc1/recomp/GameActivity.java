package com.dkc1.recomp;

import org.libsdl.app.SDLActivity;

/** Runs the native game (libmain.so) through SDL2. */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
