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

/**
 *
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class CommandMain
{
    /*---------------------------------------------------------------
     * Constructor
     *-------------------------------------------------------------*/
    private CommandMain()
    {
    }
    
    /*---------------------------------------------------------------
     * Main Method
     *-------------------------------------------------------------*/
    public static void main(String[] args)
    {
        if ( args.length < 1 )
        {
            System.out.println( Main.getRes().getString( "Missing required command." ) );
            System.exit( 1 );
            return;
        }
        
        String command = args[0].toLowerCase();
        if ( command.equals( "halt0" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "Runtime.getRuntime().halt( 0 )" );
            Runtime.getRuntime().halt( 0 );
            return;
        }
        else if ( command.equals( "halt1" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "Runtime.getRuntime().halt( 1 )" );
            Runtime.getRuntime().halt( 1 );
            return;
        }
        else if ( command.equals( "exit0" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "System.exit( 0 )" );
            System.exit( 0 );
            return;
        }
        else if ( command.equals( "exit1" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "System.exit( 1 )" );
            System.exit( 1 );
            return;
        }
        else if ( command.equals( "stop0" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "WrapperManager.stop( 0 )" );
            WrapperManager.stop( 0 );
            return;
        }
        else if ( command.equals( "stop1" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "WrapperManager.stop( 1 )" );
            WrapperManager.stop( 1 );
            return;
        }
        else if ( command.equals( "stopandreturn0" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "WrapperManager.stopAndReturn( 0 )" );
            WrapperManager.stopAndReturn( 0 );
            return;
        }
        else if ( command.equals( "stopandreturn1" ) )
        {
            System.out.println( Main.getRes().getString( "Calling: " ) + "WrapperManager.stopAndReturn( 1 )" );
            WrapperManager.stopAndReturn( 1 );
            return;
        }
        else
        {
            System.out.println( Main.getRes().getString( "Unexpected command ''{0}''.", command ) );
            System.exit( 1 );
            return;
        }
    }
}
