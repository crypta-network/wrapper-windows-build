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
public class ExecSignals
{
    private static void doTest( String command, String testId, long timeoutMS, boolean newProcessGroup, int startType, int expectedExitCode, long expectedTimeMs )
    {
        RuntimeExec.beginCase( testId );
        try
        {
            if ( !WrapperProcessConfig.isSupported( startType ) ) 
            {
                System.out.println( Main.getRes().getString( "{0} startType {1} not supported", testId, Integer.toString( startType ) ) );
                return;
            }
            
            WrapperProcessConfig wrapperProcessConfig = new WrapperProcessConfig().setNewProcessGroup( newProcessGroup ).setAutoCloseInputStreams( m_autoClose ).setStartType( startType );
            try
            {
                RuntimeExec.handleWrapperProcess( testId, command, wrapperProcessConfig, timeoutMS, true, false, false, RuntimeExec.WAIT_MODE_MANUAL, expectedExitCode, m_debug );
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
    
    private static boolean m_bashForwardsSignals = false;
    private static boolean m_debug = false;
    private static boolean m_autoClose = true;
    private static final boolean m_powershellPresent = new File( "C:\\Windows\\System32\\WindowsPowerShell" ).exists();
    
    /** Time after launching a child process before it is crashed. */
    private static final int CRASH_DELAY_S = 1;
    private static final long CRASH_DELAY_MS = CRASH_DELAY_S * 1000L;
    
    /** Time after launching a child process that we send a TERM or CTRL-C asking it to stop cleanly. */
    private static final int CTRL_C_DELAY_S = 2;
    private static final long CTRL_C_DELAY_MS = CTRL_C_DELAY_S * 1000L;
    
    /** The time that we ask the child process to run before exiting on its own. */
    private static final int CHILD_SLEEP_TIME_S = 10;
    private static final long CHILD_SLEEP_TIME_MS = CHILD_SLEEP_TIME_S * 1000L;
    
    /** Time the Wrapper will wait after sending a TERM or CTRL-C to the child before giving up and killing the process. */
    private static final int CHILD_KILL_TIMEOUT_S = 5;
    private static final long CHILD_KILL_TIMEOUT_MS = CHILD_KILL_TIMEOUT_S * 1000L;
    
    private static void doTests( String executable, boolean newProcessGroup, int startType )
    {
        boolean executableExists = ( new java.io.File( executable ) ).exists();
        StringBuffer sb = new StringBuffer();
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
        
        
        //
        // 1) executable is called directly (no intermediate bash process)
        //
        if ( executableExists )
        {
            doTest( executable + " 3 " + CHILD_SLEEP_TIME_S, context + "NoIgnore Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, CTRL_C_DELAY_MS );
            doTest( executable + " -ignoresignals 3 " + CHILD_SLEEP_TIME_S, context + "Ignore Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_KILLED, CTRL_C_DELAY_MS + ( WrapperManager.isWindows() ? 0 : CHILD_KILL_TIMEOUT_MS ) );
            doTest( executable + " -crash 3 " + CRASH_DELAY_S, context + "NoIgnore Crash : ", 0, newProcessGroup, startType, RuntimeExec.EXIT_CODE_CRASH, CRASH_DELAY_MS );
            doTest( executable + " -ignoresignals -crash 3 " + CRASH_DELAY_S, context + "Ignore Crash  : ", 0, newProcessGroup, startType, RuntimeExec.EXIT_CODE_CRASH, CRASH_DELAY_MS );
        }
        else
        {
            doTest( executable + " 3 " + CHILD_SLEEP_TIME_S, context + "NoIgnore Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_COMMAND_NOT_FOUND, 0 );
        }
        
        
        //
        // 2) executable is called via an intermediate bash, batch or powershell process - we need to distinguish Unix and Windows.
        //
        if ( WrapperManager.isWindows() )
        {
            //
            // 2-1) executable is simplewaiter
            //        NOTE: these tests on Windows will run for the full time. simplewaiter is not killed and pipes stay opened until it stops.
            //              Should we add an option to force terminate the full process tree, similar to what we do on UNIX ?
            //              (see TODO comments in destroyChildObject() and callTheReaperPid())
            //
            if ( executableExists )
            {
                //
                // 2-1-1) simplewaiter with default signal handler
                //
                doTest( "cmd /c \"" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "CMD Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, CHILD_SLEEP_TIME_MS );
                
                if ( m_powershellPresent )
                {
                    doTest( "powershell.exe \"" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "PowerShell Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, CHILD_SLEEP_TIME_MS );
                }

                //
                // 2-1-2) simplewaiter with custom signal handler ignoring signals
                //
                doTest( "cmd /c \"" + executable + " -ignoresignals 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "CMD Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, CHILD_SLEEP_TIME_MS );

                if ( m_powershellPresent )
                {
                    doTest( "powershell.exe \"" + executable + " -ignoresignals 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "PowerShell Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, CHILD_SLEEP_TIME_MS );
                }
            }
            // TODO: create a simplewaiterw (consoless app) which should be able to receive CTRL-C signals - ideally with and without an hidden window receiving WM_CLOSE messages.
            
            //
            // 2-2) executable doesn't exist
            //
            else
            {
                // Should always complete quickly as the pipes will be closed together with the bash process.
                doTest( "cmd /c \"" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_COMMAND_NOT_FOUND, 0 );
            }
        }
        else
        {
            //
            // 2-1) executable is simplewaiter
            //
            if ( executableExists )
            {
                //
                // 2-1-1) simplewaiter with default signal handler
                //

                //
                // 2-1-1-1) bash runs simplewaiter as a single command.
                //
                doTest( "bash -c \"" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, CTRL_C_DELAY_MS );
                
                //
                // 2-1-1-2) bash runs simplewaiter as part of a series: export FOO=bar; ./simplewaiter ...
                //          NOTE: we observe a different behavior from case 2-1-1-1) but the underneath reason is not clearly understood
                //
                if ( !WrapperManager.isZOS() )
                {
                    long expectedTimeMs;
                    if ( m_bashForwardsSignals ) // On some systems bash behaves like 2-1-1-1) even when commands are in series.
                    {
                        // bash will get the TERM signal and forwards it.
                        expectedTimeMs = CTRL_C_DELAY_MS;
                    }
                    else if ( newProcessGroup )
                    {
                        // bash and executable will get the TERM signal and will exit.
                        expectedTimeMs = CTRL_C_DELAY_MS;
                    }
                    else if ( m_autoClose )
                    {
                        // the read() method will stop blocking when bash process terminates. Shortly after the reaper will close the InputStreams.
                        expectedTimeMs = CTRL_C_DELAY_MS;
                    }
                    else // sharedProcessGroup
                    {
                        // bash will get the TERM signal, but not the executable.  The executable is never notified so it will run for the full time.
                        //  The test case is expected to take the full amount of time as the output pipes will not be closed when bash is terminated.
                        expectedTimeMs = CHILD_SLEEP_TIME_MS;
                    }
                    doTest( "bash -c \"export FOO=bar;" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash+env Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, expectedTimeMs );
                }
                else
                {
                    // On zOS, stdout/stderr seem to be only attached to bash, not to the sub-child 'simplewaiter' process. Simplewaiter will be running for CHILD_SLEEP_TIME_S secs, but the pipes will be closed when bash terminates and won't block until simplewaiter completes.
                    //  If simplewaiter is launched in a new process group, it will be monitored together with bash and should respond to CTRL-C in the same delay.
                    doTest( "bash -c \"export FOO=bar;" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash+env Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, CTRL_C_DELAY_MS );
                }
                
                //
                // 2-1-2) simplewaiter with custom signal handler ignoring signals
                //

                //
                // 2-1-2-1) bash runs simplewaiter as a single command.
                //          NOTE: a TERM signal will not kill bash until simplewaiter exits.  So the timeout happens and they are both killed with SIGKILL.  We get EXIT_CODE_KILLED.
                //
                doTest( "bash -c \"" + executable + " -ignoresignals 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash Ignore Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_KILLED, CTRL_C_DELAY_MS + CHILD_KILL_TIMEOUT_MS );
                
                doTest( "bash -c \"" + executable + " -crash 3 " + CRASH_DELAY_S  + "\"", context + "Bash NoIgnore Crash : ", 0, newProcessGroup, startType, RuntimeExec.EXIT_CODE_CRASH, CRASH_DELAY_MS );
                doTest( "bash -c \"" + executable + " -ignoresignals -crash 3 " + CRASH_DELAY_S  + "\"", context + "Bash Ignore Crash  : ", 0, newProcessGroup, startType, RuntimeExec.EXIT_CODE_CRASH, CRASH_DELAY_MS );
                
                //
                // 2-1-2-2) bash runs simplewaiter as part of a series: export FOO=bar; ./simplewaiter ...
                //          NOTE: we observe a different behavior from case 2-1-2-1) but the underneath reason is not clearly understood
                //
                if ( !WrapperManager.isZOS() )
                {
                    long expectedTimeMs;
                    int expectedExitCode;
                    if ( m_bashForwardsSignals ) // On some systems bash behaves like 2-1-1-1) even when commands are in series.
                    {
                        // bash will get the TERM signal and forwards it.
                        //  bash will exit when it gets the TERM signal, but the executable will ignore it and continue to wait until it times out and is killed.
                        expectedTimeMs = CTRL_C_DELAY_MS + CHILD_KILL_TIMEOUT_MS;
                        expectedExitCode = RuntimeExec.EXIT_CODE_KILLED;
                    }
                    else
                    {
                        if ( newProcessGroup )
                        {
                            // bash and executable will get the TERM signal.
                            //  bash will exit when it gets the TERM signal, but the executable will ignore it and continue to wait until it times out and is killed.
                            expectedTimeMs = CTRL_C_DELAY_MS + CHILD_KILL_TIMEOUT_MS;
                        }
                        else if ( m_autoClose )
                        {
                            // the read() method will stop blocking when bash process terminates. Shortly after the reaper will close the InputStreams.
                            expectedTimeMs = CTRL_C_DELAY_MS;
                        }
                        else // sharedProcessGroup
                        {
                            // bash will get the TERM signal, but not the executable.  The executable is never notified so it will run for the full time.
                            //  The test case is expected to take the full amount of time as the output pipes will not be closed when bash is terminated.
                            expectedTimeMs = CHILD_SLEEP_TIME_MS;
                        }

                        // a TERM signal kills bash and we get EXIT_CODE_TERM_CTRL_C.  But the simplewaiter times out and is SIGKILLed.
                        expectedExitCode = RuntimeExec.EXIT_CODE_TERM_CTRL_C;
                    }
                    doTest( "bash -c \"export FOO=bar;" + executable + " -ignoresignals 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash+env Ignore Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, expectedExitCode, expectedTimeMs );
                }
                else
                {
                    // On zOS, stdout/stderr seem to be only attached to bash, not to the sub-child 'simplewaiter' process. Simplewaiter will be running for CHILD_SLEEP_TIME_S secs, but the pipes will be closed when bash terminates and won't block until simplewaiter completes.
                    //  If simplewaiter is launched in a new process group, it will be monitored together with bash but will ignore CTRL-C. An additional CHILD_KILL_TIMEOUT_MS delay is expected to happen until the process is terminated.
                    doTest( "bash -c \"export FOO=bar;" + executable + " -ignoresignals 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash+env Ignore Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_TERM_CTRL_C, newProcessGroup ? CTRL_C_DELAY_MS + CHILD_KILL_TIMEOUT_MS : CTRL_C_DELAY_MS );
                }
                
                doTest( "bash -c \"export FOO=bar;" + executable + " -crash 3 " + CRASH_DELAY_S  + "\"", context + "Bash+env NoIgnore Crash : ", 0, newProcessGroup, startType, RuntimeExec.EXIT_CODE_CRASH, CRASH_DELAY_MS );
                doTest( "bash -c \"export FOO=bar;" + executable + " -ignoresignals -crash 3 " + CRASH_DELAY_S  + "\"", context + "Bash+env Ignore Crash  : ", 0, newProcessGroup, startType, RuntimeExec.EXIT_CODE_CRASH, CRASH_DELAY_MS );
            }
            //
            // 2-2) executable doesn't exist
            //
            else
            {
                // Should always complete quickly as the pipes will be closed together with the bash process.
                doTest( "bash -c \"" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType, RuntimeExec.EXIT_CODE_COMMAND_NOT_FOUND, 0 );
                
                doTest( "bash -c \"export FOO=bar;" + executable + " 3 " + CHILD_SLEEP_TIME_S  + "\"", context + "Bash+env Destroy " + CTRL_C_DELAY_S + "s : ", CTRL_C_DELAY_MS, newProcessGroup, startType,  RuntimeExec.EXIT_CODE_COMMAND_NOT_FOUND, 0 );
            }
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
        final String doesnotexist;
        
        if ( WrapperManager.isWindows() )
        {
            simplewaiter = "..\\test\\simplewaiter.exe";
            doesnotexist = "..\\test\\doesnotexist.exe";
        }
        else
        {
            simplewaiter = "../test/simplewaiter";
            doesnotexist = "../test/doesnotexist";
        }
        
        if ( !WrapperManager.isWindows() )
        {
            if ( ( args.length > 0 ) && "true".equals( args[0] ) )
            {
                System.out.println( Main.getRes().getString( "Detected that Bash forwards signals to its child processes." ) );
                m_bashForwardsSignals = true;
            }
            if ( ( args.length > 1 ) )
            {
                if ( "true".equals( args[1] ) )
                {
                    System.out.println( Main.getRes().getString( "Using Auto-close mode to read the pipes." ) );
                    m_autoClose = true;
                }
                else
                {
                    System.out.println( Main.getRes().getString( "Using Blocking mode to read the pipes." ) );
                    m_autoClose = false;
                }
            }
            if ( ( args.length > 2 ) && "true".equals( args[2] ) )
            {
                System.out.println( Main.getRes().getString( "DEBUG mode enabled." ) );
                m_debug = true;
            }
        }
        
        doTests( simplewaiter, true, WrapperProcessConfig.DYNAMIC );
        doTests( doesnotexist, true, WrapperProcessConfig.DYNAMIC );
        if ( !WrapperManager.isWindows() )
        {
            doTests( simplewaiter, false, WrapperProcessConfig.DYNAMIC );
            doTests( doesnotexist, false, WrapperProcessConfig.DYNAMIC );
            
            if ( WrapperProcessConfig.isSupported( WrapperProcessConfig.FORK_EXEC ) )
            {
                doTests( simplewaiter, true, WrapperProcessConfig.FORK_EXEC );
                doTests( simplewaiter, false, WrapperProcessConfig.FORK_EXEC );
                doTests( doesnotexist, true, WrapperProcessConfig.FORK_EXEC );
                doTests( doesnotexist, false, WrapperProcessConfig.FORK_EXEC );
            }
            
            if ( WrapperProcessConfig.isSupported( WrapperProcessConfig.VFORK_EXEC ) )
            {
                doTests( simplewaiter, true, WrapperProcessConfig.VFORK_EXEC );
                doTests( simplewaiter, false, WrapperProcessConfig.VFORK_EXEC );
                doTests( doesnotexist, true, WrapperProcessConfig.VFORK_EXEC );
                doTests( doesnotexist, false, WrapperProcessConfig.VFORK_EXEC );
            }
            
            if ( WrapperProcessConfig.isSupported( WrapperProcessConfig.POSIX_SPAWN ) )
            {
                doTests( simplewaiter, true, WrapperProcessConfig.POSIX_SPAWN );
                doTests( simplewaiter, false, WrapperProcessConfig.POSIX_SPAWN );
                doTests( doesnotexist, true, WrapperProcessConfig.POSIX_SPAWN );
                doTests( doesnotexist, false, WrapperProcessConfig.POSIX_SPAWN );
            }
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
