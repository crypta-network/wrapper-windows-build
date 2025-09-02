package org.tanukisoftware.wrapper.testmodule.nativeaccess;

import java.lang.ClassLoader;
import java.lang.Module;

public class Main
{
    public static void main(String[] args)
    {
        String baseName = System.getProperty( "wrapper.native_library" );
        String libPath = System.getProperty( "java.library.path" );

        System.out.println( "Loading " + libPath + "/" + baseName + "..." );
        try
        {
            System.loadLibrary( baseName );
            System.out.println( "Loaded!" );
        }
        catch ( Throwable e )
        {
            System.err.println( e.toString() );
        }
    }
}
