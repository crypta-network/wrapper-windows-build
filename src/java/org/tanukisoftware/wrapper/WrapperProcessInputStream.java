package org.tanukisoftware.wrapper;

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

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.io.IOException;

/**
 * The InputStream Class of a WrapperProcess, representing all the data the
 * ChildProcess writes to the Wrapper.
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 * @since Wrapper 3.4.0
 */
public class WrapperProcessInputStream
    extends InputStream
{
    /** Platform specific pointer to the underlying native pipe being read from. */
    private long m_ptr;
    
    /** Flag which keeps track of whether the close method has been called. */
    private boolean m_closed;
    
    /** Buffer containing all unread data from the pipe.  This will only exist if the process terminates before a read method has been called. */
    private ByteArrayInputStream m_bais;
    
    /** Read flag is set the first time a thread calls any of the read methods.
     *   This is used to decide whther or not pending data in the buffer needs to be stored in m_bais once the process is terminated.
     *   Storing the unread data is necessary if the child process terminates before the use code has a chance to access the InputStreams.
     */
    private boolean m_read;
    
    /** Flag which is set when the child process is terminated.  Used within native code. */
    private volatile boolean m_terminated;
    
    /** Indicates if the pipes should be read in a non-blocking mode with check of the m_terminated flag between retries.  Used within native code. */
    private volatile boolean m_checkTerminated;
    
    /** Semaphore to synchronize access to the m_read flag.
     *   It is important that the synchronization is made on a distinct object so that m_read can be accessed while other threads hold a lock on 'this'.
     */
    private Object m_readSm = new Object();

    /*---------------------------------------------------------------
     * Constructors
     *-------------------------------------------------------------*/
    /**
     * This class can only be instantiated by native code.
     */
    private WrapperProcessInputStream()
    {
    }

    /*---------------------------------------------------------------
     * Native Methods
     *-------------------------------------------------------------*/
    private native int nativeAvailable();
    private native int nativeRead( boolean blocking );
    private native void nativeClose();
    private native int nativeRead2( byte[] b, int off, int len );

    /*---------------------------------------------------------------
     * Methods
     *-------------------------------------------------------------*/
    /**
     * Returns an estimate of the number of bytes that can be read (or skipped over) from this input stream without blocking by the next invocation of a method for this input stream.
     *  The next invocation might be the same thread or another thread. A single read or skip of this many bytes will not block, but may read or skip fewer bytes.
     *
     * @return An estimate of the number of bytes that can be read (or skipped over) from this input stream without blocking or 0 when it reaches the end of the input stream.
     *
     * @throws IOException If an I/O error occurs.
     */
    public int available()
        throws IOException
    {
        synchronized( this )
        {
            if ( ( !m_closed ) && WrapperManager.isNativeLibraryOk() )
            {
                return nativeAvailable();
            }
            else
            {
                if ( m_bais != null )
                {
                    return m_bais.available();
                }
                else
                {
                    // Stream is closed.
                    return 0;
                }
            }
        }
    }
    
    /**
     * Closes the InputStream
     * @throws IOException in case of any file errors
     * @throws WrapperLicenseError If the function is called other than in
     *                             the Professional Edition or from a Standalone JVM.
     */
    public void close()
        throws IOException
    {
        synchronized( this )
        {
            if ( !m_closed )
            {
                if ( WrapperManager.isNativeLibraryOk() )
                {
                    nativeClose();
                }
                m_closed = true;
            }
        }
    }

    /**
     * Tests if this input stream supports the mark and reset methods.
     *
     * The markSupported method of InputStream returns false.
     *
     * @return false.
     */
    public boolean markSupported()
    {
        return false;
    }

    /**
     * Returns TRUE if the backend connection is open or there is still any output available in buffered input.
     *
     * @return TRUE if ready, otherwise FALSE.
     *
     * @deprecated As this is a non-standard InputStream method.
     */
    public boolean ready()
    {
        synchronized( this )
        {
            if ( !m_closed || ( ( m_bais != null ) && ( m_bais.available() > 0 ) ) )
            {
                return true;
            }
            else
            {
                return false;
            }
        }
    }

    /**
     * Read a character from the Stream and moves the position in the stream
     *
     * @return A single character from the stream, or -1 when the end of stream is reached.
     *
     * @throws IOException in case the stream has been already closed or any other file error
     * @throws WrapperLicenseError If the function is called other than in
     *                             the Professional Edition or from a Standalone JVM.
     */
    public int read()
        throws IOException
    {
        if ( !m_read )
        {
            synchronized ( m_readSm )
            {
                m_read = true;
            }
        }

        synchronized( this )
        {
            if ( ( !m_closed ) && WrapperManager.isNativeLibraryOk() )
            {
                return nativeRead( true );
            }
            else
            {
                if ( m_bais != null )
                {
                    return m_bais.read();
                }
                else
                {
                    throw new IOException( WrapperManager.getRes().getString( "Stream is closed." ) );
                }
            }
        }
    }

    /**
     * Reads some number of bytes from the input stream and stores them into the buffer array b.
     *  The number of bytes actually read is returned as an integer.  This method blocks until input
     *  data is available, end of file is detected, or an exception is thrown.
     *
     * If the length of b is zero, then no bytes are read and 0 is returned; otherwise, there is an
     *  attempt to read at least one byte. If no byte is available because the stream is at the end
     *  of the file, the value -1 is returned; otherwise, at least one byte is read and stored into b.
     *
     * The first byte read is stored into element b[0], the next one into b[1], and so on. The number
     *  of bytes read is, at most, equal to the length of b. Let k be the number of bytes actually
     *  read; these bytes will be stored in elements b[0] through b[k-1], leaving elements b[k]
     *  through b[b.length-1] unaffected.
     *
     * The read(b) method for class InputStream has the same effect as:
     *
     *  read(b, 0, b.length)
     *
     * @param b The buffer into which the data is read.
     *
     * @return The total number of bytes read into the buffer, or -1 is there is no more data because
     *         the end of the stream has been reached.
     *
     * @throws IOException If the first byte cannot be read for any reason other than the end of the
     *                     file, if the input stream has been closed, or if some other I/O error
     *                     occurs.
     * @throws WrapperLicenseError If the function is called other than in
     *                             the Professional Edition or from a Standalone JVM.
     */
    public int read( byte b[ ] )
        throws IOException
    {
        return read( b, 0, b.length );
    }

    /**
     * Reads up to len bytes of data from the input stream into an array of bytes. An attempt is made
     *  to read as many as len bytes, but a smaller number may be read. The number of bytes actually
     *  read is returned as an integer.
     *
     * This method blocks until input data is available, end of file is detected, or an exception is
     *  thrown.
     *
     * If len is zero, then no bytes are read and 0 is returned; otherwise, there is an attempt to
     *  read at least one byte. If no byte is available because the stream is at end of file, the
     *  value -1 is returned; otherwise, at least one byte is read and stored into b.
     *
     * The first byte read is stored into element b[off], the next one into b[off+1], and so on. The
     *  number of bytes read is, at most, equal to len. Let k be the number of bytes actually read;
     *  these bytes will be stored in elements b[off] through b[off+k-1], leaving elements b[off+k]
     *  through b[off+len-1] unaffected.
     *
     * In every case, elements b[0] through b[off] and elements b[off+len] through b[b.length-1] are
     *  unaffected.
     *
     * @param b The buffer into which the data is read.
     * @param off The start offset in array b from which the data is read.
     * @param len The maximum number of bytes to read.
     *
     * @return The total number of bytes read into the buffer, or -1 is there is no more data because
     *         the end of the stream has been reached.
     *
     * @throws IOException If the first byte cannot be read for any reason other than the end of the
     *                     file, if the input stream has been closed, or if some other I/O error
     *                     occurs.
     * @throws WrapperLicenseError If the function is called other than in
     *                             the Professional Edition or from a Standalone JVM.
     */
    public int read( byte b[], int off, int len )
        throws IOException
    {
        if ( !m_read )
        {
            synchronized ( m_readSm )
            {
                m_read = true;
            }
        }

        synchronized ( this )
        {
            if ( b == null )
            {
                throw new NullPointerException();
            }
            else if ( ( off < 0 ) || ( len < 0 ) || ( len > b.length - off ) )
            {
                throw new IndexOutOfBoundsException();
            }
            else if ( len == 0 )
            {
                return 0;
            }
//            if ( !ready() )
//            {
//                return -1;
//            }
            int result;
            if ( ( !m_closed ) && WrapperManager.isNativeLibraryOk() )
            {
                result = nativeRead2( b, off, len );
            }
            else
            {
                if ( m_bais != null )
                {
                    result = m_bais.read( b, off, len );
                }
                else
                {
                    throw new IOException(WrapperManager.getRes().getString( "Stream is closed." ) );
                }
            }
            return result;
        }
    }

    /*---------------------------------------------------------------
     * Private Methods
     *-------------------------------------------------------------*/
    /**
     * This method gets called when a spawned Process has terminated
     *  and the pipe buffer gets read and stored in an byte array.
     *  This way we can close the Filedescriptor and keep the number
     *  of open FDs as small as possible.
     */
    private void readAndCloseOpenFDs()
    {
        // Set the terminated flag.  This must be done even when read has already been called.
        //  It is also important that this be set even if another thread is currently reading.
        m_terminated = true;
        
        synchronized ( m_readSm )
        {
            if ( m_read )
            {
                // Another thread is reading from the stream so we can trust that thread to complete the reads and close on its own.
                return;
            }
        }

        synchronized( this )
        {
            int count;
            int msg;
            if ( m_closed || ( !WrapperManager.isNativeLibraryOk() ) )
            {
                return;
            }
            try
            {
                byte[] buffer = new byte[1024];
                count = 0;
                while ( ( msg = nativeRead( false ) ) != -1 )
                {
                    if ( count >= buffer.length )
                    {
                        byte[] temp = new byte[buffer.length + 1024];
                        System.arraycopy( buffer, 0, temp, 0, buffer.length );
                        buffer = temp;
                    }
                    buffer[count++] = (byte)msg;
                }
                m_bais = new ByteArrayInputStream( buffer, 0, count );
                close();
            }
            catch( IOException ioe )
            {
                System.out.println( WrapperManager.getRes().getString( "WrapperProcessStream encountered a ReadError: " ) );
                ioe.printStackTrace();
            }
        }
    }
}
