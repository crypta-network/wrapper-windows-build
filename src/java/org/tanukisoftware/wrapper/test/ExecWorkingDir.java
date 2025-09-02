package org.tanukisoftware.wrapper.test;

/*
 * Copyright (c) 1999, 2025 Tanuki Software, Ltd.
 * http://www.tanukisoftware.com
 * All rights reserved.
 *
 * This software is the proprietary information of Tanuki Software.
 * You shall use it only in accordance with the terms of the
 * license agreement you entered into with Tanuki Software.
 * http://wrapper.tanukisoftware.com/doc/english/licenseOverview.html
 */

import java.io.File;
import org.tanukisoftware.wrapper.WrapperManager;
import org.tanukisoftware.wrapper.WrapperProcessConfig;

/**
 *
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class ExecWorkingDir
{
    private static void doTest( String command, String chDir, int startType, boolean spawnChDir )
    {
        StringBuffer sb = new StringBuffer( "'" + chDir + "' " );
        if ( startType == WrapperProcessConfig.POSIX_SPAWN )
        {
            sb.append( "PosixSpawn" );
        }
        else if ( startType == WrapperProcessConfig.FORK_EXEC )
        {
            sb.append( "Fork" );
        }
        else if ( startType == WrapperProcessConfig.VFORK_EXEC )
        {
            sb.append( "VFork" );
        }
        else
        {
            sb.append( "Dynamic" );
        }
        sb.append( " " );
        if ( spawnChDir )
        {
            sb.append( "(spawnChDir) " );
        }
        String testId = sb.toString();

        RuntimeExec.beginCase( testId );
        try
        {
            if ( !WrapperProcessConfig.isSupported( startType ) ) 
            {
                System.out.println( Main.getRes().getString( "{0} startType {1} not supported", testId, Integer.toString( startType ) ) );
                return;
            }

            try
            {
                WrapperProcessConfig wrapperProcessConfig = new WrapperProcessConfig().setWorkingDirectory( new File( chDir ) ).setStartType( startType );
                RuntimeExec.handleWrapperProcess( testId, command, wrapperProcessConfig, 0, true, false, false, RuntimeExec.WAIT_MODE_API, 0, false );
            }
            catch ( Exception e )
            {
                e.printStackTrace();
            }
        }
        finally
        {
            RuntimeExec.endCase( testId, 0 );
        }
    }

    /*---------------------------------------------------------------
     * Main Method
     *-------------------------------------------------------------*/
    public static void main( String[] args )
    {
        final String command;

        if ( WrapperManager.isWindows() )
        {
            command = "cmd /c cd";
        }
        else
        {
            command = "pwd";
        }

        System.out.println( "" );
        System.out.println( Main.getRes().getString( "Please check that each command run in its correct directory." ) );

        /* test stdout */
        doTest( command, ".",  WrapperProcessConfig.DYNAMIC, false );
        doTest( command, "..", WrapperProcessConfig.DYNAMIC, false );
        if ( !WrapperManager.isWindows() )
        {
            doTest( command, ".",  WrapperProcessConfig.POSIX_SPAWN, false );
            doTest( command, "..", WrapperProcessConfig.POSIX_SPAWN, false );
            doTest( command, ".",  WrapperProcessConfig.FORK_EXEC,   false );
            doTest( command, "..", WrapperProcessConfig.FORK_EXEC,   false );
            doTest( command, ".",  WrapperProcessConfig.VFORK_EXEC,  false );
            doTest( command, "..", WrapperProcessConfig.VFORK_EXEC,  false );

            System.setProperty( "wrapper.child.allowCWDOnSpawn", "TRUE" );
            doTest( command, ".",  WrapperProcessConfig.DYNAMIC,     true );
            doTest( command, "..", WrapperProcessConfig.DYNAMIC,     true );
            doTest( command, ".",  WrapperProcessConfig.POSIX_SPAWN, true );
            doTest( command, "..", WrapperProcessConfig.POSIX_SPAWN, true );
        }
    }
}
