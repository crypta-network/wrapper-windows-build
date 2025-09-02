package org.tanukisoftware.wrapper.testmodule.wrapper;

import java.lang.ClassLoader;
import java.lang.Module;
import org.tanukisoftware.wrapper.testmodule.base.TestApp;
import org.tanukisoftware.wrapper.WrapperManager;
import org.tanukisoftware.wrapper.event.WrapperEvent;
import org.tanukisoftware.wrapper.event.WrapperEventListener;

public class Main
    implements WrapperEventListener
{
    private static TestApp app;
    
    public static void main(String[] args)
    {
        if ( args.length > 0 && "use_wrapper".equals( args[0] ) )
        {
            WrapperManager.addWrapperEventListener( new Main(), WrapperEventListener.EVENT_FLAG_CONTROL );
        }
        app = new TestApp();
        app.start( Main.class.getModule() );
    }

    /*---------------------------------------------------------------
     * WrapperEventListener Methods
     *-------------------------------------------------------------*/
    public void fired( WrapperEvent event )
    {
        System.out.println( "Main class received event: " + event );
        app.stop();
    }
}
