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

// import java.lang.management.ThreadMXBean; // Require Java 5+
// import java.lang.management.ManagementFactory; // Require Java 5+

import org.tanukisoftware.wrapper.WrapperManager;
import org.tanukisoftware.wrapper.WrapperProcessConfig;

/**
 *
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class ExecParallel
{
    /** The time that we ask the child process to run before exiting on its own. */
    private static String CMD;
    private static String EXECUTABLE;
    private static final int CHILD_SLEEP_TIME_S = 4;
    private static final long CHILD_SLEEP_TIME_MS = CHILD_SLEEP_TIME_S * 1000L;
    // private static ThreadMXBean mbean; // Require Java 5+
    // private static volatile long cpuTime; // Require Java 5+
    private static String[] testIds = new String[6];
    private static long[] totalSysTime = new long[6];
    // private static long[] totalCpuTime = new long[6]; // Require Java 5+
    private static int seriesIndex = 0;

    static
    {
        if ( WrapperManager.isWindows() )
        {
            EXECUTABLE = "..\\test\\simplewaiter.exe";
            CMD = "CMD";
        }
        else
        {
            EXECUTABLE = "../test/simplewaiter";
            CMD = "Bash";
        }
        // mbean = ManagementFactory.getThreadMXBean(); // Require Java 5+
    }

    private static void doTestInner( String command, String testId, boolean newProcessGroup )
    {
        WrapperProcessConfig wrapperProcessConfig = new WrapperProcessConfig().setNewProcessGroup( newProcessGroup );
        try
        {
            RuntimeExec.handleWrapperProcess( testId, command, wrapperProcessConfig, 0, false, false, false, RuntimeExec.WAIT_MODE_API, 0, false );
        }
        catch ( Exception e )
        {
            e.printStackTrace();
        }
    }

    private static void doTest( String testId, boolean newProcessGroup, boolean useCmd, boolean detached )
    {
        if ( !useCmd ) 
        {
            doTestInner( EXECUTABLE + " 0 " + CHILD_SLEEP_TIME_S, testId, newProcessGroup );
        }
        else if ( !detached )
        {
            if ( WrapperManager.isWindows() )
            {
                doTestInner( "cmd /c \"" + EXECUTABLE + " 0 " + CHILD_SLEEP_TIME_S  + "\"", testId, newProcessGroup );
            }
            else
            {
                doTestInner( "bash -c \"" + EXECUTABLE + " 0 " + CHILD_SLEEP_TIME_S + "\"", testId, newProcessGroup );
            }
        }
        else
        {
            if ( WrapperManager.isWindows() )
            {
                doTestInner( "cmd /c \"start /B " + EXECUTABLE + " 0 " + CHILD_SLEEP_TIME_S  + "\"", testId, newProcessGroup );
            }
            else
            {
                doTestInner( "bash -c \"" + EXECUTABLE + " 0 " + CHILD_SLEEP_TIME_S + " &\"", testId, newProcessGroup );
            }
        }
    }

    private static void doTestsParallel( final boolean newProcessGroup, final boolean useCmd, final boolean detached )
    {
        String testIdBase;
        long expectedTime;

        if ( newProcessGroup )
        {
            testIdBase = "NewProcessGroup|";
        }
        else
        {
            testIdBase = "";
        }
        if ( !useCmd ) 
        {
            testIdBase += "simplewaiter ";
            expectedTime = CHILD_SLEEP_TIME_MS;
        }
        else if ( !detached )
        {
            testIdBase += CMD + "+simplewaiter ";
            expectedTime = CHILD_SLEEP_TIME_MS;
        }
        else
        {
            testIdBase += CMD + "+simplewaiter(detached) ";
            expectedTime = newProcessGroup ? CHILD_SLEEP_TIME_MS : 0;
        }

        for (int i = 0; i < 10; i += 2)   /* 1 4 16 64 256 */
        {
            int n = (int)Math.pow( 2, i );
            final String testId = testIdBase + "(" + n + " threads) :";
            RuntimeExec.beginCase( testId );

            Thread[] threads = new Thread[n];

            // cpuTime = 0; // Require Java 5+
            for (int j = 0; j < n; j++)
            {
                // mbean = ManagementFactory.getThreadMXBean(); // Require Java 5+
                threads[j] = new Thread()
                {
                    public void run()
                    {
                        doTest( testId, newProcessGroup, useCmd, detached );
                        // cpuTime += mbean.getThreadCpuTime( Thread.currentThread().getId() ); // Require Java 5+
                    }
                };
            }
            long sysTime = System.currentTimeMillis();
            for (int j = 0; j < n; j++)
            {
                threads[j].start();
            }
            for (int j = 0; j < n; j++)
            {
                try
                {
                    threads[j].join();
                }
                catch ( InterruptedException e )
                {
                }
            }
            sysTime = System.currentTimeMillis() - sysTime;
            // cpuTime /= 1000000; /* ns -> ms */ // Require Java 5+

            // RuntimeExec.endCase( testId + " (System time: " + sysTime + "ms, CPU time: " + cpuTime + "ms)", expectedTime ); // Require Java 5+
            RuntimeExec.endCase( testId + " (System time: " + sysTime + "ms)", expectedTime );
            totalSysTime[seriesIndex] += sysTime;
            // totalCpuTime[seriesIndex] += cpuTime; // Require Java 5+
        }
        testIds[seriesIndex] = testIdBase;
        seriesIndex++;
    }

    /*---------------------------------------------------------------
     * Main Method
     *-------------------------------------------------------------*/
    public static void main( String[] args )
    {
        // Need to initialize the counts.
        RuntimeExec.c_testsPerformed = 0;
        RuntimeExec.c_testsFailed = 0;

        System.out.println( "" );
        System.out.println( Main.getRes().getString( "Running test with wrapper.child.wait_signals=" + System.getProperty( "wrapper.child.wait_signals" ) ) );
        System.out.println( Main.getRes().getString( "Running test with wrapper.child.wait_signals.max_threads=" + System.getProperty( "wrapper.child.wait_signals.max_threads" ) ) );
        System.out.println( Main.getRes().getString( " You may change the values of these properties in the execparallel.conf." ) );

        doTestsParallel( false, false, false );
        doTestsParallel( false, true, false );
        doTestsParallel( false, true, true );
        doTestsParallel( true, false, false );
        doTestsParallel( true, true, false );
        doTestsParallel( true, true, true );

        int nbTestsFailed = RuntimeExec.c_testsFailed;
        int nbTestsPassed = RuntimeExec.c_testsPerformed - nbTestsFailed;

        System.out.println( "" );
        System.out.println( Main.getRes().getString( "[PASSED] {0}", Integer.toString( nbTestsPassed ) ) );
        System.out.println( Main.getRes().getString( "[FAILED] {0}", Integer.toString( nbTestsFailed ) ) );
        System.out.println( Main.getRes().getString( "--------------------" ));
        for (int i = 0; i < testIds.length; i++) {
            // System.out.println( Main.getRes().getString( "{0}: System time: {1}ms, CPU time: {2}ms", testIds[i], Long.toString( totalSysTime[i] ), Long.toString( totalCpuTime[i] ) ) ); // Require Java 5+
            System.out.println( Main.getRes().getString( "{0}: System time: {1}ms", testIds[i], Long.toString( totalSysTime[i] ) ) );
        }
        System.out.println( Main.getRes().getString( "--------------------" ));

        if ( nbTestsFailed > 0 )
        {
            System.exit( 1 );
        }
    }
}
