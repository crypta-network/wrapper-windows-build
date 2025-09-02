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

import org.tanukisoftware.wrapper.WrapperManager;
import org.tanukisoftware.wrapper.WrapperProcessConfig;

/**
 * This class tests the ability to write to the output stream of a subprocess
 *  and to read the input stream of that same process. A comparison is then
 *  made to confirm they both match.
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class ExecWriteAndRead
{    
    private static boolean m_debug = false;
    
    /** The time that we ask the child process to run before exiting on its own. */
    private static final int CHILD_SLEEP_TIME_S = 0;
    private static final long CHILD_SLEEP_TIME_MS = CHILD_SLEEP_TIME_S * 1000L;
    private static final String CHILD_INPUT_STRING = "some input..." + System.getProperty("line.separator");

    private static void doTest( String command, String testId, boolean newProcessGroup, int startType, long expectedTimeMs )
    {
        RuntimeExec.beginCase( testId );
        try
        {
            WrapperProcessConfig wrapperProcessConfig = new WrapperProcessConfig().setNewProcessGroup( newProcessGroup ).setStartType( startType );
            try
            {
                RuntimeExec.handleWrapperProcess( testId, command, wrapperProcessConfig, 0, true, false, CHILD_INPUT_STRING, false, RuntimeExec.WAIT_MODE_MANUAL, "(.*)(" + CHILD_INPUT_STRING + ")(.*)", 0, m_debug, false );
            }
            catch ( Exception e )
            {
                e.printStackTrace();
            }
        }
        finally
        {
            RuntimeExec.endCase( testId, expectedTimeMs );
        }
    }
    
    private static void doTests( String executable, boolean newProcessGroup, int startType )
    {
        StringBuffer sb = new StringBuffer();
        if ( newProcessGroup )
        {
            sb.append( "NewProcessGroup" );
        }
        else
        {
            sb.append( "SharedProcessGroup" );
        }
        sb.append( " " );
        String context = sb.toString();
        
        // launching simplewaiter only. Expected to read for CHILD_SLEEP_TIME_MS.
        doTest( executable + " -readinput 0 " + CHILD_SLEEP_TIME_S, context + "simplewaiter : ", newProcessGroup, startType, CHILD_SLEEP_TIME_MS );

        if ( !WrapperManager.isWindows() )
        {
            // launching bash as direct child and simplewaiter as a subchild. Bash will exit after simplewaiter completes. Expected to read for CHILD_SLEEP_TIME_MS.
            doTest( "bash -c \"" + executable + " -readinput 0 " + CHILD_SLEEP_TIME_S + "\"", context + "Bash+simplewaiter : ", newProcessGroup, startType, CHILD_SLEEP_TIME_MS );

            // launching bash as direct child and simplewaiter as a subchild. Bash will exit after simplewaiter completes. Expected to read for CHILD_SLEEP_TIME_MS.
            doTest( "bash -c \"export FOO=bar;" + executable + " -readinput 0 " + CHILD_SLEEP_TIME_S + "\"", context + "Bash+env+simplewaiter : ", newProcessGroup, startType, CHILD_SLEEP_TIME_MS );
        }
    }
    
    /*---------------------------------------------------------------
     * Main Method
     *-------------------------------------------------------------*/
    public static void main( String[] args )
    {
        // Need to initialize the counts.
        RuntimeExec.c_testsPerformed = 0;
        RuntimeExec.c_testsFailed = 0;
        
        final String simplewaiter;

        if ( WrapperManager.isWindows() )
        {
            simplewaiter = "..\\test\\simplewaiter.exe";
        }
        else
        {
            simplewaiter = "../test/simplewaiter";
        }
        
        if ( !WrapperManager.isWindows() )
        {
            if ( ( args.length > 0 ) && "true".equals( args[0] ) )
            {
                System.out.println( Main.getRes().getString( "DEBUG mode enabled." ) );
                m_debug = true;
            }
        }

        System.out.println( "" );
        System.out.println( Main.getRes().getString( "A bunch of tests will be run. Please check that the input written matches the output printed." ) );
        
        /* test stdin */
        if ( WrapperProcessConfig.isSupported( WrapperProcessConfig.FORK_EXEC ) )
        {
            doTests( simplewaiter, true, WrapperProcessConfig.FORK_EXEC );
            doTests( simplewaiter, false, WrapperProcessConfig.FORK_EXEC );
        }
        
        if ( WrapperProcessConfig.isSupported( WrapperProcessConfig.VFORK_EXEC ) )
        {
            doTests( simplewaiter, true, WrapperProcessConfig.VFORK_EXEC );
            doTests( simplewaiter, false, WrapperProcessConfig.VFORK_EXEC );
        }
        
        if ( WrapperProcessConfig.isSupported( WrapperProcessConfig.POSIX_SPAWN ) )
        {
            doTests( simplewaiter, true, WrapperProcessConfig.POSIX_SPAWN );
            doTests( simplewaiter, false, WrapperProcessConfig.POSIX_SPAWN );
        }
        
        int nbTestsFailed = RuntimeExec.c_testsFailed;
        int nbTestsPassed = RuntimeExec.c_testsPerformed - nbTestsFailed;
        
        System.out.println( "" );
        System.out.println( Main.getRes().getString( "[PASSED] {0}", Integer.toString( nbTestsPassed ) ) );
        System.out.println( Main.getRes().getString( "[FAILED] {0}", Integer.toString( nbTestsFailed ) ) );
        
        if ( nbTestsFailed > 0 )
        {
            System.exit( 1 );
        }
    }
}
