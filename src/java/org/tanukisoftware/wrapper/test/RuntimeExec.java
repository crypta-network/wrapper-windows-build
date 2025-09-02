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

import org.tanukisoftware.wrapper.WrapperJNIError;
import org.tanukisoftware.wrapper.WrapperLicenseError;
import org.tanukisoftware.wrapper.WrapperManager;
import org.tanukisoftware.wrapper.WrapperProcess;
import org.tanukisoftware.wrapper.WrapperProcessConfig;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.IOException;
import java.util.Random;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 *
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class RuntimeExec
{
    /* Normal = 0. */
    public static final int EXIT_CODE_NORMAL = 0;
    
    /* Error = 1. */
    public static final int EXIT_CODE_ERROR = 1;
    
    /* On UNIX Command not found = 127. */
    public static final int EXIT_CODE_COMMAND_NOT_FOUND = ( WrapperManager.isWindows() ? 1 : 127 );
    
    /* On UNIX Signal 15(TERM) + 128 = 143. */
    public static final int EXIT_CODE_TERM_CTRL_C = ( WrapperManager.isWindows() ? 1 : 143 );
    
    /* On UNIX Signal 9(KILL) + 128 = 137. */
    public static final int EXIT_CODE_KILLED = ( WrapperManager.isWindows() ? 1 : 137 );
    
    /* On Mac Signal 4(CORE) + 128 = 132, other UNIX Signal 11(SEGFAULT) + 128 = 139. */
    public static final int EXIT_CODE_CRASH = ( WrapperManager.isWindows() ? -1073741819 : ( WrapperManager.isMacOSX() ? 132 : 139 ) );
    
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
    
    static Thread handleOutputStream( final OutputStream os, final String encoding, final String text, final String label )
    {
        Thread runner = new Thread()
        {
            public void run()
            {
                BufferedWriter bw;
                
                try
                {
                    System.out.println( label );
                    bw = new BufferedWriter( new OutputStreamWriter( os, encoding ) );
                    try
                    {
                        bw.write( text, 0, text.length() );
                    }
                    finally
                    {
                        bw.close();
                    }
                }
                catch ( IOException e )
                {
                    e.printStackTrace();
                }
            }
        };
        runner.start();
        
        return runner;
    }
    
    static Thread handleInputStream( final InputStream is, final String encoding, final String label, final StringBuffer sb )
    {
        Thread runner = new Thread()
        {
            public void run()
            {
                BufferedReader br;
                String line;
                
                try
                {
                    br = new BufferedReader( new InputStreamReader( is, encoding ) );
                    try
                    {
                        boolean firstLine = true;
                        while ( ( line = br.readLine() ) != null )
                        {
                            if ( sb != null )
                            {
                                if ( firstLine )
                                {
                                    firstLine = false;
                                }
                                else
                                {
                                    sb.append( System.getProperty("line.separator") );
                                }
                                sb.append( line );
                            }
                            System.out.println( label + " " + line );
                        }
                    }
                    finally
                    {
                        br.close();
                    }
                }
                catch ( IOException e )
                {
                    e.printStackTrace();
                }
            }
        };
        runner.start();
        
        return runner;
    }
    
    static Thread handleInputStreamByteByByte( final InputStream is, final String label, final StringBuffer sb )
    {
        Thread runner = new Thread()
        {
            public void run()
            {
                try
                {
                    int c;
                    char chr;
                    boolean printLabel = true;
                    while ( ( c = is.read() ) != -1 )
                    {
                        if ( sb != null )
                        {
                            sb.append( c );
                        }
                        chr = (char)c;
                        if ( printLabel )
                        {
                            System.out.print( label + " " );
                            printLabel = false;
                        }
                        System.out.print( chr );
                        
                        if ( chr == '\n' )
                        {
                            printLabel = true;
                        }
                    }
                }
                catch ( IOException e )
                {
                    e.printStackTrace();
                }
            }
        };
        runner.start();
        
        return runner;
    }
    
    static void handleJavaProcessInner( String testId, Process process )
        throws IOException, InterruptedException
    {
        try
        {
            handleInputStream( process.getInputStream(), c_encoding, testId + "stdout:", null );
            handleInputStream( process.getErrorStream(), c_encoding, testId + "stderr:", null );
        }
        finally
        {
            int exitCode = process.waitFor();
            System.out.println( Main.getRes().getString( "{0}exitCode: {1}", testId, new Integer( exitCode ) ) );
        }
    }
    
    static void handleJavaProcess( String testId, String command )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}Runtime.exec command: {1}", testId, command ) );
        handleJavaProcessInner( testId, Runtime.getRuntime().exec( command ) );
    }
    
    static void handleJavaProcess( String testId, String[] command )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}Runtime.exec command: {1}", testId, toString( command ) ) );
        handleJavaProcessInner( testId, Runtime.getRuntime().exec( command ) );
    }

    static final int WAIT_MODE_NONE = 0;
    static final int WAIT_MODE_API = 1;
    static final int WAIT_MODE_MANUAL = 2;
    
    static BufferedReader debugStdOut;

    /**
     * @param timeoutMS Time to wait before attempting to call process.destroy(), 0 for never.
     */
    private static void handleWrapperProcessInner( String testId, WrapperProcess process, boolean newProcessGroup, long timeoutMS, boolean handleOutErr, boolean closeStdOutErr, String textStdIn, boolean closeStdIn, int waitMode, String outErrRegex, int expectedExitCode, boolean debug, boolean readByteByByte )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}  Launched PID={1}, PGID={2}", testId, Integer.toString( process.getPID() ), Integer.toString( process.getPGID() ) ) );
        
        if ( debug )
        {
            final long sleepTime = timeoutMS * 2/3;
            final int pgid = process.getPGID();
            new Thread()
            {
                public void run()
                {
                    try
                    {
                        Thread.sleep( sleepTime );

                        String[] cmd = {
                            "/bin/sh",
                            "-c",
                            "ps -o pid,ppid,pgid,comm | grep -iE '" + pgid + "|pid' | grep -Ev ' sh| ps| grep'"
                        };
                        Process proc = Runtime.getRuntime().exec( cmd );
                        
                        debugStdOut = new BufferedReader( new InputStreamReader( proc.getInputStream()) );
                    }
                    catch ( IOException e )
                    {
                    }
                    catch ( InterruptedException e )
                    {
                    }
                }
            }.start();
        }

        if ( newProcessGroup )
        {
            // PID should equal PGID.
            if ( process.getPID() != process.getPGID() )
            {
                System.out.println( Main.getRes().getString( "{0}    FAILED! A new process group was requested, but the child process does not have its own group.", testId ) );
                c_currentTestFailed = true;
            }
        }
        else
        {
            // PID should not equal PGID.
            if ( process.getPID() == process.getPGID() )
            {
                System.out.println( Main.getRes().getString( "{0}    FAILED! A new process group was not requested, but the child process has a new group.", testId ) );
                c_currentTestFailed = true;
            }
        }
        
        Thread stdoutThread = null;
        Thread stderrThread = null;
        StringBuffer sbOut = null;
        StringBuffer sbErr = null;
        try
        {
            if ( textStdIn != null )
            {
                handleOutputStream( process.getStdIn(), c_encoding, textStdIn, testId + "stdin: " + textStdIn );
            }
            
            if ( handleOutErr )
            {
                if ( outErrRegex != null )
                {
                    // create a StringBuffer to collect the output.
                    sbOut = new StringBuffer();
                    sbErr = new StringBuffer();
                }
                
                if ( readByteByByte )
                {
                    stdoutThread = handleInputStreamByteByByte( process.getStdOut(), testId + "stdout:", sbOut );
                    stderrThread = handleInputStreamByteByByte( process.getStdErr(), testId + "stderr:", sbErr );
                }
                else
                {
                    stdoutThread = handleInputStream( process.getStdOut(), c_encoding, testId + "stdout:", sbOut );
                    stderrThread = handleInputStream( process.getStdErr(), c_encoding, testId + "stderr:", sbErr );
                }
            }
            else if ( closeStdOutErr )
            {
                process.getStdOut().close();
                process.getStdErr().close();
            }
            
            if ( closeStdIn )
            {
                process.getStdIn().close();
            }
            
            if ( waitMode == WAIT_MODE_API )
            {
                // We always call waitFor later.
            }
            else if ( waitMode == WAIT_MODE_MANUAL )
            {
                if ( timeoutMS > 0 )
                {
                    long start = System.currentTimeMillis();
                    while ( process.isAlive() && ( System.currentTimeMillis() - start < timeoutMS ) )
                    {
                        try
                        {
                            Thread.sleep( 100 );
                        }
                        catch ( InterruptedException e )
                        {
                        }
                    }
                    long time =  System.currentTimeMillis() - start;
                    if ( process.isAlive() )
                    {
                        System.out.println( Main.getRes().getString( "{0}Timed out after {1}ms waiting for child.  Destroying.", testId, Long.toString( time ) ) );
                        process.destroy();
                    }
                    else if ( System.currentTimeMillis() - start >= timeoutMS )
                    {
                        // We timed out, so the child should still exist.
                        System.out.println( Main.getRes().getString( "{0}FAILED! Timed out after {1}ms waiting for child.  But child is already gone unexpectedly.", testId, Long.toString( time ) ) );
                        c_currentTestFailed = true;
                    }
                    else
                    {
                        // The child was gone before the timeout.  This is normal if the child was expected to fail, but bad if not.
                        System.out.println( Main.getRes().getString( "{0}Child completed within {1}ms, before timeout of {2}ms.", testId, Long.toString( time ), Long.toString( timeoutMS ) ) );
                    }
                }
            }
        }
        finally
        {
            if ( ( waitMode == WAIT_MODE_API ) || ( waitMode == WAIT_MODE_MANUAL ) )
            {
                int exitCode = process.waitFor();
                if ( exitCode == expectedExitCode )
                {
                    System.out.println( Main.getRes().getString( "{0}exitCode: {1}", testId, new Integer( exitCode ) ) );
                }
                else
                {
                    System.out.println( Main.getRes().getString( "{0}FAILED! exitCode: {1} but expected {2}", testId, new Integer( exitCode ), new Integer( expectedExitCode ) ) );
                    c_currentTestFailed = true;
                }
                
                long start = System.currentTimeMillis();
                if ( stdoutThread != null )
                {
                    System.out.println( Main.getRes().getString( "{0}waiting for stdout thread...", testId ) );
                    try
                    {
                        stdoutThread.join();
                        long time = System.currentTimeMillis() - start;
                        if ( time > 10 )
                        {
                            System.out.println( Main.getRes().getString( "{0}stdout thread didn't complete for " + time + "ms. after child process exited.", testId ) );
                        }
                        else
                        {
                            System.out.println( Main.getRes().getString( "{0}stdout thread complete promptly.", testId ) );
                        }
                    }
                    catch ( InterruptedException e )
                    {
                        System.out.println( Main.getRes().getString( "{0}wait for stdout thread interrupted.", testId ) );
                    }
                }
                
                if ( stderrThread != null )
                {
                    System.out.println( Main.getRes().getString( "{0}waiting for stderr thread...", testId ) );
                    try
                    {
                        stderrThread.join();
                        long time = System.currentTimeMillis() - start;
                        if ( time > 10 )
                        {
                            System.out.println( Main.getRes().getString( "{0}stderr thread didn't complete for " + time + "ms. after child process exited.", testId ) );
                        }
                        else
                        {
                            System.out.println( Main.getRes().getString( "{0}stderr thread complete promptly.", testId ) );
                        }
                    }
                    catch ( InterruptedException e )
                    {
                        System.out.println( Main.getRes().getString( "{0}wait for stderr thread interrupted.", testId ) );
                    }
                }
            }
            else
            {
                System.out.println( Main.getRes().getString( "{0}leave running...", testId ) );
            }

            if ( ( outErrRegex != null ) && ( sbOut != null ) && ( sbErr != null ) )
            {
                Pattern pattern = Pattern.compile( outErrRegex, Pattern.DOTALL | Pattern.MULTILINE );

                if ( pattern.matcher( sbOut ).matches() )
                {
                    System.out.println( Main.getRes().getString( "{0}Expected output found in {1}.", testId, "stdout" ) );
                }
                else if ( pattern.matcher( sbErr ).matches() )
                {
                    System.out.println( Main.getRes().getString( "{0}Expected output found in {1}.", testId, "stderr" ) );
                }
                else
                {
                    System.out.println( Main.getRes().getString( "{0}FAILED! Neither stdout nor stderr matched ''{1}''.", testId, outErrRegex ) );
                    c_currentTestFailed = true;
                }
            }

            if ( debug )
            {
                System.out.println( "----------------DEBUG----------------" );
                String s = null;
                while ( ( s = debugStdOut.readLine() ) != null )
                {
                    System.out.println( s );
                }
                System.out.println( "-------------------------------------" );
            }
        }
    }

    static void handleWrapperProcess( String testId, String command, WrapperProcessConfig config, long timeoutMS, boolean handleOutErr, boolean closeStdOutErr, String textStdIn, boolean closeStdIn, int waitMode, String outErrRegex, int expectedExitCode, boolean debug, boolean readByteByByte )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}WrapperManager.exec command: {1}", testId, command ) );
        if ( config == null )
        {
            handleWrapperProcessInner( testId, WrapperManager.exec( command ), true, timeoutMS, handleOutErr, closeStdOutErr, textStdIn, closeStdIn, waitMode, outErrRegex, expectedExitCode, debug, readByteByByte );
        }
        else
        {
            handleWrapperProcessInner( testId, WrapperManager.exec( command, config ), config.isNewProcessGroup(), timeoutMS, handleOutErr, closeStdOutErr, textStdIn, closeStdIn, waitMode, outErrRegex, expectedExitCode, debug, readByteByByte );
        }
    }
    
    static void handleWrapperProcess( String testId, String command, WrapperProcessConfig config, long timeoutMS, boolean handleOutErr, boolean closeStdOutErr, boolean closeStdIn, int waitMode, int expectedExitCode, boolean debug, boolean readByteByByte )
        throws IOException, InterruptedException
    {
        handleWrapperProcess( testId, command, config, timeoutMS, handleOutErr, closeStdOutErr, null, closeStdIn, waitMode, null, expectedExitCode, debug, false );
    }
    
    static void handleWrapperProcess( String testId, String command, WrapperProcessConfig config, long timeoutMS, boolean handleOutErr, boolean closeStdOutErr, boolean closeStdIn, int waitMode, int expectedExitCode, boolean debug )
        throws IOException, InterruptedException
    {
        handleWrapperProcess( testId, command, config, timeoutMS, handleOutErr, closeStdOutErr, null, closeStdIn, waitMode, null, expectedExitCode, debug, false );
    }
    
    private static void handleWrapperProcess( String testId, String command )
        throws IOException, InterruptedException
    {
        handleWrapperProcess( testId, command, null, 0, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_NORMAL, false );
    }

    private static void handleWrapperProcess( String testId, String[] command, WrapperProcessConfig config, long timeoutMS, boolean handleOutErr, boolean closeStdOutErr, boolean closeStdIn, int waitMode, int expectedExitCode )
        throws IOException, InterruptedException
    {
        System.out.println( Main.getRes().getString( "{0}WrapperManager.exec command: {1}", testId, toString( command ) ) );
        if ( config == null )
        {
            handleWrapperProcessInner( testId, WrapperManager.exec( command ), true, timeoutMS, handleOutErr, closeStdOutErr, null, closeStdIn, waitMode, null, expectedExitCode, false, false );
        }
        else
        {
            handleWrapperProcessInner( testId, WrapperManager.exec( command, config ), config.isNewProcessGroup(), timeoutMS, handleOutErr, closeStdOutErr, null, closeStdIn, waitMode, null, expectedExitCode, false, false );
        }
    }
    private static void handleWrapperProcess( String testId, String[] command )
        throws IOException, InterruptedException
    {
        handleWrapperProcess( testId, command, null, 0, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_NORMAL );
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
    
    private static void caseLongCommand( final String simplewaiter, int len, boolean expectFailure )
    {
        
        String testId = "Long Command " + len + ": ";
        beginCase( testId );
        try
        {
            try
            {
                StringBuffer sb = new StringBuffer();
                sb.append( simplewaiter );
                sb.append( " -v " );
                while ( sb.length() < len - 1 )
                {
                    sb.append( "x" );
                }
                // Make the last character a y so we can verify that it is included correctly.
                sb.append( "y" );
                String command = sb.toString();
                
                handleWrapperProcess( testId, command );
                if ( expectFailure )
                {
                    System.out.println( Main.getRes().getString( "{0}Was looking for a failure, but the long command line passed.  This is likely because the limit is larger on this platform.", testId ) );
                }
            }
            catch ( Exception e )
            {
                if ( expectFailure )
                {
                    System.out.println( Main.getRes().getString( "{0}Failed as expected: {1}", testId, e.toString() ) );
                }
                else
                {
                    System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                    e.printStackTrace();
                    c_currentTestFailed = true;
                }
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseWaitFor( final String simplewaiter )
    {
        String testId = "WaitFor : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " 1 10";
                handleWrapperProcess( testId, command, null, 0, true, false, false, WAIT_MODE_API, EXIT_CODE_ERROR, false );
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
    
    private static void caseSmallChildProcess( final String simplewaiter )
    {
        String testId = "Simple Wrapper (Array) : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " 65 1";
                handleWrapperProcess( testId, command, null, 0, false, false, true, WAIT_MODE_MANUAL, 65, false );
            }
            catch ( InterruptedException e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
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
    
    private static void caseVFork( final String simplewaiter )
    {
        String testId = "VFork : ";
        beginCase( testId );
        try
        {
            if ( !WrapperProcessConfig.isSupported( WrapperProcessConfig.VFORK_EXEC ) ) 
            {
                System.out.println( Main.getRes().getString( "{0}vfork not supported", testId ) );
            }
            else
            {
                System.out.println( Main.getRes().getString( "{0}vfork is supported", testId ) );
                try
                {
                    String command = simplewaiter + " 20 10";
                    handleWrapperProcess( testId, command, new WrapperProcessConfig().setStartType( WrapperProcessConfig.VFORK_EXEC ), 0, true, false, false, WAIT_MODE_MANUAL, 20, false );
                }
                catch ( Exception e )
                {
                    System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                    e.printStackTrace();
                    c_currentTestFailed = true;
                }
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void casePosixSpawn( final String simplewaiter )
    {
        String testId = "PosixSpawn : ";
        beginCase( testId );
        try
        {
            if ( !WrapperProcessConfig.isSupported( WrapperProcessConfig.POSIX_SPAWN ) ) 
            {
                System.out.println( Main.getRes().getString( "{0}posix spawn not supported", testId ) );
            }
            else
            {
                System.out.println( Main.getRes().getString( "{0}posix spawn is supported", testId ) );
                try
                {
                    String command = simplewaiter + " 20 10";
                    handleWrapperProcess( testId, command, new WrapperProcessConfig().setStartType( WrapperProcessConfig.POSIX_SPAWN ), 0, true, false, false, WAIT_MODE_MANUAL, 20, false );
                }
                catch ( Exception e )
                {
                    System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                    e.printStackTrace();
                    c_currentTestFailed = true;
                }
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseEnvSmall( final String simplewaiter )
    {
        String testId = "Environment Small : ";
        beginCase( testId );
        try
        {
            try
            {
                WrapperProcessConfig config = new WrapperProcessConfig();
                java.util.Map environment = config.getEnvironment();
                environment.clear();
                environment.put( "TEST", "TEST123" );
                System.out.println( Main.getRes().getString( "{0}size of Environment map = {1}", testId, new Integer( environment.size() ) ) );
                
                String command = simplewaiter + " -e TEST";
                handleWrapperProcess( testId, command, config, 0, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_NORMAL, false );
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
    
    private static void caseEnvLarge( final String simplewaiter, int len, boolean expectFailure )
    {
        String testId = "Environment Large " + len + ": ";
        beginCase( testId );
        try
        {
            try
            {
                int valueLen = len - 4 - 1; // "TEST="
                StringBuffer sb = new StringBuffer();
                for ( int i = 0; i < valueLen - 1; i++ )
                {
                    sb.append( "X" );
                }
                sb.append( "Y" ); // Make it so we can see the last value.
                String value = sb.toString();
                
                WrapperProcessConfig config = new WrapperProcessConfig();
                java.util.Map environment = config.getEnvironment();
                environment.clear();
                environment.put( "TEST", value );
                System.out.println( Main.getRes().getString( "{0}size of Environment map = {1}", testId, new Integer( environment.size() ) ) );
                
                String command = simplewaiter + " -e TEST";
                handleWrapperProcess( testId, command, config, 0, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_NORMAL, false );
                if ( expectFailure )
                {
                    System.out.println( Main.getRes().getString( "{0}Was looking for a failure, but the long environment variable passed.  This is likely because the limit is larger on this platform.", testId ) );
                }
            }
            catch ( Exception e )
            {
                if ( expectFailure )
                {
                    System.out.println( Main.getRes().getString( "{0}Failed as expected: {1}", testId, e.toString() ) );
                }
                else
                {
                    System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                    e.printStackTrace();
                    c_currentTestFailed = true;
                }
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseWorkingDir( final String simplewaiter )
    {
        String testId = "Change Working Dir : ";
        beginCase( testId );
        try
        {
            if ( WrapperProcessConfig.isSupported( WrapperProcessConfig.FORK_EXEC ) || WrapperProcessConfig.isSupported( WrapperProcessConfig.VFORK_EXEC ) )
            {
                System.out.println( Main.getRes().getString( "{0}changing the working directory is supported", testId ) );
                try
                {
                    WrapperProcessConfig config = new WrapperProcessConfig();
                    config.setStartType( WrapperProcessConfig.isSupported( WrapperProcessConfig.FORK_EXEC ) ? WrapperProcessConfig.FORK_EXEC : WrapperProcessConfig.VFORK_EXEC );
                    config.setWorkingDirectory( new File("..") );
                    
                    String command;
                    if ( WrapperManager.isWindows() )
                    {
                        command = "cmd.exe /c dir";
                    }
                    else
                    {
                        command = "ls -l";
                    }
                    handleWrapperProcess( testId, command, config, 0, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_NORMAL, false );
                }
                catch ( Exception e )
                {
                    System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                    e.printStackTrace();
                    c_currentTestFailed = true;
                }
            }
            else
            {
                System.out.println( Main.getRes().getString( "{0}changing the working directory is not supported", testId ) );
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    /**
     * Test a short WrapperManager.exec process whose entire lifespan is while another Runtime.exec process is running.
     */
    private static void caseWrapperDuringJava( final String simplewaiter )
    {
        String testId = "Wrapper During Java : ";
        beginCase( testId );
        try
        {
            try
            {
                String javaCommand = simplewaiter + " 5 10";
                String wrapperCommand = simplewaiter + " 6 5";
                
                Process javaProcess = Runtime.getRuntime().exec( javaCommand );
                handleInputStream( javaProcess.getInputStream(), c_encoding, testId + "Runtime.exec stdout:", null );
                handleInputStream( javaProcess.getErrorStream(), c_encoding, testId + "Runtime.exec stderr:", null );
                
                WrapperProcess wrapperProcess = WrapperManager.exec( wrapperCommand );
                handleInputStream( wrapperProcess.getStdOut(), c_encoding, testId + "WrapperManager.exec stdout:", null );
                handleInputStream( wrapperProcess.getStdErr(), c_encoding, testId + "WrapperManager.exec stderr:", null );

                System.out.println( Main.getRes().getString( "{0}WrapperManager.exec exitCode: {1}", testId, new Integer( wrapperProcess.waitFor() ) ) );
                
                System.out.println( Main.getRes().getString( "{0}Runtime.exec exitCode: {1}", testId, new Integer( javaProcess.waitFor() ) ) );
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
    
    /**
     * Test a short Runtime.exec process whose entire lifespan is while another WrapperManager.exec process is running.
     */
    private static void caseJavaDuringWrapper( final String simplewaiter )
    {
        String testId = "Java During Wrapper : ";
        beginCase( testId );
        try
        {
            try
            {
                String wrapperCommand = simplewaiter + " 5 10";
                String javaCommand = simplewaiter + " 6 5";
                
                WrapperProcess wrapperProcess = WrapperManager.exec( wrapperCommand );
                handleInputStream( wrapperProcess.getStdOut(), c_encoding, testId + "WrapperManager.exec stdout:", null );
                handleInputStream( wrapperProcess.getStdErr(), c_encoding, testId + "WrapperManager.exec stderr:", null );

                Process javaProcess = Runtime.getRuntime().exec( javaCommand );
                handleInputStream( javaProcess.getInputStream(), c_encoding, testId + "Runtime.exec stdout:", null );
                handleInputStream( javaProcess.getErrorStream(), c_encoding, testId + "Runtime.exec stderr:", null );
                
                System.out.println( Main.getRes().getString( "{0}Runtime.exec exitCode: {1}", testId, new Integer( javaProcess.waitFor() ) ) );
                
                System.out.println( Main.getRes().getString( "{0}WrapperManager.exec exitCode: {1}", testId, new Integer( wrapperProcess.waitFor() ) ) );
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
    
    private static void caseInvalid( final String simplewaiter )
    {
        String testId = "Invalid : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = "invalid";
                handleWrapperProcess( testId, command, null, 0, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_COMMAND_NOT_FOUND, false );
                System.out.println( Main.getRes().getString( "{0}ERROR! Did not fail as expected.", testId ) );
                c_currentTestFailed = true;
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}Failed as expected.", testId ) );
            }
        }
        finally
        {
            endCase( testId );
        }
    }
    
    private static void caseTimeoutShort( final String simplewaiter )
    {
        String testId = "Timeout Short : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " 0 5";
                handleWrapperProcess( testId, command, null, 10000, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_NORMAL, false );
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
    
    private static void caseTimeoutLong( final String simplewaiter )
    {
        String testId = "Timeout Long : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " 0 30";
                handleWrapperProcess( testId, command, null, 10000, true, false, false, WAIT_MODE_MANUAL, EXIT_CODE_TERM_CTRL_C, false );
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
    
    private static boolean caseLeaveRunning( final String simplewaiter )
    {
        String testId = "Leave Running : ";
        beginCase( testId );
        try
        {
            try
            {
                String command = simplewaiter + " 1 600";
                handleWrapperProcess( testId, command, null, 0, true, false, false, WAIT_MODE_NONE, EXIT_CODE_ERROR, false );
                return false;
            }
            catch ( WrapperJNIError e )
            {
                System.out.println( Main.getRes().getString( "{0}Unable to launch child process because JNI library unavailable. Normal on shutdown.", testId ) );
                return true;
            }
            catch ( Exception e )
            {
                System.out.println( Main.getRes().getString( "{0}ERROR - Unexpected error:", testId ) );
                e.printStackTrace();
                c_currentTestFailed = true;
                return true;
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
        
        System.out.println( "Communicate with child processes using encoding: " + c_encoding );
        
        Random rand = new Random();
        System.out.println( Main.getRes().getString( "Is DYNAMIC supported? A:" ) + WrapperProcessConfig.isSupported( WrapperProcessConfig.DYNAMIC ) );
        System.out.println( Main.getRes().getString( "Is FORK_EXEC supported? A:" ) + WrapperProcessConfig.isSupported( WrapperProcessConfig.FORK_EXEC ) );
        System.out.println( Main.getRes().getString( "Is VFORK_EXEC supported? A:" ) + WrapperProcessConfig.isSupported( WrapperProcessConfig.VFORK_EXEC ) );
        System.out.println( Main.getRes().getString( "Is POSIX_SPAWN supported? A:" ) + WrapperProcessConfig.isSupported( WrapperProcessConfig.POSIX_SPAWN ) );

        caseSimpleTestJava( simplewaiter );
        caseSimpleTestWrapper( simplewaiter );
        caseSimpleTestJavaAry( simplewaiter );
        caseSimpleTestWrapperAry( simplewaiter );
        
        caseLongCommand( simplewaiter, 32766, false );
        caseLongCommand( simplewaiter, 32767, true );
        
        caseWaitFor( simplewaiter );
        
        caseSmallChildProcess( simplewaiter );
        
        caseVFork( simplewaiter );
        casePosixSpawn( simplewaiter );
        
        caseEnvSmall( simplewaiter );
        caseEnvLarge( simplewaiter, 32767, false );
        caseEnvLarge( simplewaiter, 32768, true );
        
        caseWorkingDir( simplewaiter );
        
        caseWrapperDuringJava( simplewaiter );
        caseJavaDuringWrapper( simplewaiter );
        
        caseInvalid( simplewaiter );
        
        caseTimeoutShort( simplewaiter );
        caseTimeoutLong( simplewaiter );
        
        // This test should be the last as it relies on the Wrapper shutting it down.
        caseLeaveRunning( simplewaiter );
        
        int nbTestsPassed = c_testsPerformed - c_testsFailed;
        
        System.out.println( "" );
        System.out.println( "(Test results for current JVM #" + WrapperManager.getJVMId() + ")" );
        System.out.println( Main.getRes().getString( "[PASSED] {0}", Integer.toString( nbTestsPassed ) ) );
        System.out.println( Main.getRes().getString( "[FAILED] {0}", Integer.toString( c_testsFailed ) ) );
        
        System.out.println();
        if ( WrapperManager.getJVMId() == 1 )
        {
            // First invocation.
            System.out.println( Main.getRes().getString( "All Done. Restarting..." ) );
            WrapperManager.restart();
        }
        else
        {
            // Second invocation.
            //  Register a long shutdownhook which will cause the Wrapper to timeout and kill the JVM.
            System.out.println( Main.getRes().getString( "All Done. Registering long shutdown hook and stopping.\nWrapper should timeout and kill the JVM, cleaning up all processes in the process." ) );
            
            Runtime.getRuntime().addShutdownHook( new Thread()
            {
                public void run() {
                    System.out.println( Main.getRes().getString( "Starting shutdown hook. Loop for 25 seconds.") );
                    System.out.println( Main.getRes().getString( "Should timeout unless this property is set: wrapper.jvm_exit.timeout=30" ) );
    
                    long start = System.currentTimeMillis();
                    boolean failed = false;
                    while ( System.currentTimeMillis() - start < 25000 )
                    {
                        if ( !failed )
                        {
                            failed = caseLeaveRunning( simplewaiter );
                            System.out.println( Main.getRes().getString( "Launched child...") );
                        }
                        
                        try
                        {
                            Thread.sleep( 250 );
                        }
                        catch ( InterruptedException e )
                        {
                            // Ignore
                        }
                    }
                    System.out.println( Main.getRes().getString( "Shutdown hook complete. Should exit now." ) );
                }
            } );
            
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
}
