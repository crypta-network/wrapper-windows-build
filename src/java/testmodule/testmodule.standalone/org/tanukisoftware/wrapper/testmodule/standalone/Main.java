package org.tanukisoftware.wrapper.testmodule.standalone;

import java.lang.ClassLoader;
import java.lang.Module;
import org.tanukisoftware.wrapper.testmodule.base.TestApp;

public class Main
{
    private static TestApp app;
    
    public static void main(String[] args)
    {
        app = new TestApp();
        app.start( Main.class.getModule() );
    }
}
