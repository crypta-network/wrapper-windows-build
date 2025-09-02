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

import java.text.DateFormat;
import java.text.SimpleDateFormat;
import java.util.Date;

/**
 * This test is to observe how the Wrapper handles the logging when its main
 *  loop is slightly faster than the frequency at which the Java application
 *  is logging. On the Wrapper side, the main loop has some logic to sleep and
 *  avoid thrashing when there is nothing to read. But this logic should adapt
 *  to the frequency of output received (the read speed shouldn't delay the
 *  write on the other end of the pipe). Each line of output has been made
 *  large enough so that the pipe can easily get full if the Wrapper is
 *  sleeping for intervals that are too long.
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class LoadedSlightlySlowedOutput {
    /*---------------------------------------------------------------
     * Main Method
     *-------------------------------------------------------------*/
    public static void main(String[] args) {
        DateFormat df = new SimpleDateFormat( "yyyyMMdd'T'HHmmssSSS" );
        
        long start = System.currentTimeMillis();
        long now = start;
        
        StringBuffer sb = new StringBuffer();
        for ( int i = 0; i < 50; i++ )
        {
            sb.append( "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890" );
        }
        String s = sb.toString();
        
        System.out.println( Main.getRes().getString( "Log a large line of ouptut every millisecond for 60 seconds..." ) );
        int line = 1;
        while ( now - start < 60000 ) {
            System.out.println( ( line++ ) + " : " + ( now - start ) + " : " + df.format( new Date( now ) ) + " : " + s );
            now = System.currentTimeMillis();
            try
            {
                Thread.sleep( 1 );
            }
            catch ( InterruptedException e )
            {
            }
        }
        System.out.println( Main.getRes().getString( "All done.") );
    }
}

