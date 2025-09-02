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
import org.tanukisoftware.wrapper.WrapperProcess;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.IOException;

/**
 * Non-blocking version of the RuntimeExec test.
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class ExecAvailable
{
    static final String c_encoding;
    static int c_testsFailed = 0;
    static boolean c_currentTestFailed = false;
    static int c_testsPerformed = 0;
    
    /*---------------------------------------------------------------
     * Static Methods
     *-------------------------------------------------------------*/
    static
    {
        // In order to read the output from some processes correctly we need to get the correct encoding.
        //  On some systems, the underlying system encoding is different than the file encoding.
        String encoding = System.getProperty( "sun.jnu.encoding" );
        if ( encoding == null )
        {
            encoding = System.getProperty( "file.encoding" );
            if ( encoding == null )
            {
                // Default to Latin1
                encoding = "Cp1252";
            }
        }
        c_encoding = encoding;
    }
    
    
    private static void handleCommonProcessInner( String testId, Object process, InputStream is, InputStream es )
        throws IOException
    {
        int exitCode;
        boolean exited = false;
        try
        {
            InputStreamReader iir = new InputStreamReader( is, c_encoding );
            InputStreamReader eir = new InputStreamReader( es, c_encoding );
            
            BufferedReader ir = new BufferedReader( iir );
            BufferedReader er = new BufferedReader( eir );
            
            // Read all available input from both streams.  Do our best to keep
            //  the output in the correct order.  Keep reading for a half second
            //  after the process exits to make sure we get it all.
            long now = System.currentTimeMillis();
            long start = now;
            long lastOutput = now;
            boolean timeout = false;
            
            // Keep track of whether or not there was any stderr output.
            boolean hadErrors = false;
            
            boolean haveStdout = false;
            boolean haveStderr = false;

            while ( ( ( !exited ) || ( now - lastOutput < 500 ) ) && ( !timeout ) )
            {
                boolean sleep = true;
                
                // Try stdout stream
                if ( ir.ready() )
                {
                    do
                    {
                        int avail = is.available();
                        boolean ready = ir.ready();
                        String line = ir.readLine();
                        if ( line != null )
                        {
                            System.out.println( testId + "stdout:" + ready + "(" + avail + "):" + line );

                            sleep = false;
                            lastOutput = System.currentTimeMillis();
                        }
                    }
                    while( ir.ready() );
                }
                
                // Try stderr stream
                if ( er.ready() )
                {
                    do
                    {
                        int avail = es.available();
                        boolean ready = er.ready();
                        String line = er.readLine();
                        if ( line != null )
                        {
                            System.out.println( testId + "stderr:" + ready + "(" + avail + "):" + line );

                            sleep = false;
                            lastOutput = System.currentTimeMillis();
                        }
                    }
                    while( er.ready() );
                }
                
                // If nothing was read from either stream then sleep.
                if ( sleep )
                {
                    try
                    {
                        Thread.sleep( 10 );
                    }
                    catch ( InterruptedException e )
                    {
                        // Ignore
                    }
                }
                
                now = System.currentTimeMillis();
                
                // See if the process has exited yet.
                if ( !exited )
                {
                    try
                    {
                        if ( process instanceof WrapperProcess )
                        {
                            exitCode = ((WrapperProcess)process).exitValue();
                        }
                        else
                        {
                            exitCode = ((Process)process).exitValue();
                        }
                        exited = true;
                        
                        // Set the lastOutput time, to get any output right
                        //  before the process completed.
                        lastOutput = now;
                    }
                    catch ( IllegalThreadStateException e )
                    {
                        // Still running, ignore.
                    }
                }
            }
        }
        finally
        {
            try
            {
                if ( process instanceof WrapperProcess )
                {
                    exitCode = ((WrapperProcess)process).waitFor();
                }
                else
                {
                    exitCode = ((Process)process).waitFor();
                }
            }
            catch ( InterruptedException e )
            {
                exitCode = -999;
            }
            System.out.println( Main.getRes().getString( "{0}exitCode: {1}", testId, new Integer( exitCode ) ) );
        }
    }
    
    private static void handleJavaProcessInner( String testId, Process process )
        throws IOException, InterruptedException
    {
        handleCommonProcessInner( testId, process, process.getInputStream(), process.getErrorStream() );
    }
    
    private static void handleJavaProcess( String testId, String command )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}Runtime.exec command: {1}", testId, command ) );
        handleJavaProcessInner( testId, Runtime.getRuntime().exec( command ) );
    }
    
    private static void handleJavaProcess( String testId, String[] command )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}Runtime.exec command: {1}", testId, toString( command ) ) );
        handleJavaProcessInner( testId, Runtime.getRuntime().exec( command ) );
    }

    private static void handlWrapperProcessInner( String testId, WrapperProcess process )
        throws IOException, InterruptedException
    {
        handleCommonProcessInner( testId, process, process.getInputStream(), process.getErrorStream() );
    }
    
    private static void handleWrapperProcess( String testId, String command )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}WrapperMaanger.exec command: {1}", testId, command ) );
        handlWrapperProcessInner( testId, WrapperManager.exec( command ) );
    }
    
    private static void handleWrapperProcess( String testId, String[] command )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}WrapperMaanger.exec command: {1}", testId, toString( command ) ) );
        handlWrapperProcessInner( testId, WrapperManager.exec( command ) );
    }
    
    private static long c_start;
    static void beginCase( String testId )
    {
        c_testsPerformed++;
        c_currentTestFailed = false;
        c_start = System.currentTimeMillis();
        System.out.println();
        System.out.println( Main.getRes().getString( "{0}BEGIN ----------------------------------------", testId ) );
    }
    
    static void endCase( String testId, long expectedTimeMs )
    {
        long time = System.currentTimeMillis() - c_start;
        
        String result;
        if ( expectedTimeMs >= 0 )
        {
            long minTime = expectedTimeMs;
            long maxTime = expectedTimeMs + 999L;
            if ( ( time < minTime ) || ( time > maxTime ) )
            {
                result = " " + Main.getRes().getString( "FAILED! Expected time to be in range of {0} ~ {1}ms.", Long.toString( minTime ), Long.toString( maxTime ) );
                c_currentTestFailed = true;
            }
            else
            {
                result = " " + Main.getRes().getString( "OK." );
            }
        }
        else
        {
            result = "";
        }
        if ( c_currentTestFailed )
        {
            c_testsFailed++;
        }
        
        System.out.println( Main.getRes().getString( "{0}END   ---------------------------------------- {1}ms.{2}", testId, Long.toString( time ), result ) );
    }
    
    static void endCase( String testId )
    {
        // Try to keep all output from the test within the the BEGIN/END block.  Most tests capture it correctly however.
        try
        {
            Thread.sleep( 1000 );
        }
        catch ( InterruptedException e )
        {
        }
        
        endCase( testId, -1 );
    }
    
    private static String toString( String[] command )
    {
        StringBuffer sb = new StringBuffer();
        sb.append( "{" );
        for ( int i = 0; i < command.length; i++ )
        {
            String arg = command[i];
            if ( i > 0 )
            {
                sb.append( ", " );
            }
            sb.append( "\"" );
            sb.append( arg );
            sb.append( "\"" );
        }
        sb.append( "}" );
        
        return sb.toString();
    }
    
    private static void caseSimpleTestJava( final String simplewaiter )
    {
        String testId = "Simple Java : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " -v \"test 123\" test 123 \"\\\"test\\\"";
                handleJavaProcess( testId, command );
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseSimpleTestWrapper( final String simplewaiter )
    {
        String testId = "Simple Wrapper : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " -v \"test 123\" test 123 \"\\\"test\\\"";
                handleWrapperProcess( testId, command );
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseSimpleTestJavaAry( final String simplewaiter )
    {
        String testId = "Simple Java (Array) : ";
        beginCase( testId );
        try
        {
            try
            {
                String[] command = { simplewaiter, "-v", "\"test 123\"", "test 123", "\"\\\"test\\\"\"" };
                handleJavaProcess( testId, command );
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseSimpleTestWrapperAry( final String simplewaiter )
    {
        String testId = "Simple Wrapper (Array) : ";
        beginCase( testId );
        try
        {
            try
            {
                String[] command = { simplewaiter, "-v", "\"test 123\"", "test 123", "\"\\\"test\\\"\"" };
                handleWrapperProcess( testId, command );
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseSleepTestJava( final String simplewaiter )
    {
        String testId = "Sleep Java : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " -message TestString -messageinterval 2000 0 5";
                handleJavaProcess( testId, command );
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseSleepTestWrapper( final String simplewaiter )
    {
        String testId = "Sleep Wrapper : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " -message TestString -messageinterval 2000 0 5";
                handleWrapperProcess( testId, command );
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
            }
        }
        finally
        {
            endCase( testId );
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
            simplewaiter = "../test/simplewaiter.exe";
        }
        else
        {
            simplewaiter = "../test/simplewaiter";
        }
        
        caseSimpleTestJava( simplewaiter );
        caseSimpleTestWrapper( simplewaiter );
        caseSimpleTestJavaAry( simplewaiter );
        caseSimpleTestWrapperAry( simplewaiter );
        
        caseSleepTestJava( simplewaiter );
        caseSleepTestWrapper( simplewaiter );
        
        
        int nbTestsPassed = c_testsPerformed - c_testsFailed;
        
        System.out.println( "" );
        System.out.println( "(Test results for current JVM #" + WrapperManager.getJVMId() + ")" );
        System.out.println( Main.getRes().getString( "[PASSED] {0}", Integer.toString( nbTestsPassed ) ) );
        System.out.println( Main.getRes().getString( "[FAILED] {0}", Integer.toString( c_testsFailed ) ) );
        
        if ( c_testsFailed > 0 )
        {
            System.exit( 1 );
        }
        else
        {
            System.exit( 0 );
        }
    }
}
