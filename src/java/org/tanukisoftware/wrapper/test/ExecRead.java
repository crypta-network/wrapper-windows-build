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
 *
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class ExecRead
{    
    private static boolean m_debug = false;
    
    /** The time that we ask the child process to run before exiting on its own. */
    private static final int CHILD_SLEEP_TIME_S = 2;
    private static final long CHILD_SLEEP_TIME_MS = CHILD_SLEEP_TIME_S * 1000L;

    private static void doTest( String command, String testId, boolean readByteByteByByte, boolean newProcessGroup, boolean autoCloseInputStreams, long expectedTimeMs )
    {
        RuntimeExec.beginCase( testId );
        try
        {
            WrapperProcessConfig wrapperProcessConfig = new WrapperProcessConfig().setNewProcessGroup( newProcessGroup ).setAutoCloseInputStreams( autoCloseInputStreams );
            try
            {
                RuntimeExec.handleWrapperProcess( testId, command, wrapperProcessConfig, 0, true, false, false, RuntimeExec.WAIT_MODE_MANUAL, 0, m_debug, readByteByteByByte );
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
    
    private static void doTests( String executable, boolean testStdErr, boolean readByteByteByByte, boolean newProcessGroup, boolean autoCloseInputStreams )
    {
        StringBuffer sb = new StringBuffer();
        if ( autoCloseInputStreams )
        {
            sb.append( "AutoClose" );
        }
        else
        {
            sb.append( "Blocking " );
        }
        sb.append( " " );
        if ( newProcessGroup )
        {
            sb.append( "NewProcessGroup" );
        }
        else
        {
            sb.append( "SharedProcessGroup" );
        }
        sb.append( " " );
        if ( readByteByteByByte )
        {
            sb.append( "Read " );
        }
        else
        {
            sb.append( "Read2" );
        }
        sb.append( " " );
        String context = sb.toString();
        
        String stdErrArg = (testStdErr ? "-messagetostderr " : "");

        // launching simplewaiter only. Expected to read for CHILD_SLEEP_TIME_MS.
        doTest( executable + " -message \"hello tanukis!\" " + stdErrArg + "-messageinterval 250 0 " + CHILD_SLEEP_TIME_S, context + "simplewaiter : ", readByteByteByByte, newProcessGroup, autoCloseInputStreams, CHILD_SLEEP_TIME_MS );

        if ( !WrapperManager.isWindows() )
        {
            // launching bash as direct child and simplewaiter as a subchild. Bash will exit after simplewaiter completes. Expected to read for CHILD_SLEEP_TIME_MS.
            doTest( "bash -c \"" + executable + " -message hello\\ tanukis! " + stdErrArg + "-messageinterval 250 0 " + CHILD_SLEEP_TIME_S + "\"", context + "Bash+simplewaiter : ", readByteByteByByte, newProcessGroup, autoCloseInputStreams, CHILD_SLEEP_TIME_MS );

            // launching bash as direct child and simplewaiter as a subchild. Bash will exit after simplewaiter completes. Expected to read for CHILD_SLEEP_TIME_MS.
            doTest( "bash -c \"export FOO=bar;" + executable + " -message hello\\ tanukis! " + stdErrArg + "-messageinterval 250 0 " + CHILD_SLEEP_TIME_S + "\"", context + "Bash+env+simplewaiter : ", readByteByteByByte, newProcessGroup, autoCloseInputStreams, CHILD_SLEEP_TIME_MS );

            // launching bash as direct child and simplewaiter as a detached subchild. Bash will exit right away. Expected to stop reading almost immediately if we are in auto-close mode and if we are not managing sub-children as group members.
            doTest( "bash -c \"" + executable + " -message hello\\ tanukis! " + stdErrArg + "-messageinterval 250 0 " + CHILD_SLEEP_TIME_S + " &\"", context + "Bash+simplewaiter(detached) : ", readByteByteByByte, newProcessGroup, autoCloseInputStreams, ( ( autoCloseInputStreams || WrapperManager.isZOS() ) && !newProcessGroup ) ? 0 : CHILD_SLEEP_TIME_MS );

            // launching bash as direct child and simplewaiter as a detached subchild. Bash will exit right away. Expected to stop reading almost immediately if we are in auto-close mode and if we are not managing sub-children as group members.
            doTest( "bash -c \"export FOO=bar;" + executable + " -message hello\\ tanukis! " + stdErrArg + "-messageinterval 250 0 " + CHILD_SLEEP_TIME_S + " &\"", context + "Bash+env+simplewaiter(detached) : ", readByteByteByByte, newProcessGroup, autoCloseInputStreams, ( ( autoCloseInputStreams || WrapperManager.isZOS() ) && !newProcessGroup ) ? 0 : CHILD_SLEEP_TIME_MS );
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
        System.out.println( Main.getRes().getString( "A bunch of tests will be run. Please check that:\n- Each of the tests that take ~2 secs to execute have standard output messages.\n- Tests that stop immediately should not have stdout messages.\nThe summary of results below only take into account the expected times of execution." ) );
        
        /* test stdout */
        doTests( simplewaiter, false, true, false, false );
        doTests( simplewaiter, false, true, false, true );
        doTests( simplewaiter, false, true, true, false );
        doTests( simplewaiter, false, true, true, true );
        doTests( simplewaiter, false, false, false, false );
        doTests( simplewaiter, false, false, false, true );
        doTests( simplewaiter, false, false, true, false );
        doTests( simplewaiter, false, false, true, true );

        /* test stderr */
        doTests( simplewaiter, true, true, false, false );
        doTests( simplewaiter, true, true, false, true );
        doTests( simplewaiter, true, true, true, false );
        doTests( simplewaiter, true, true, true, true );
        doTests( simplewaiter, true, false, false, false );
        doTests( simplewaiter, true, false, false, true );
        doTests( simplewaiter, true, false, true, false );
        doTests( simplewaiter, true, false, true, true );
        
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
