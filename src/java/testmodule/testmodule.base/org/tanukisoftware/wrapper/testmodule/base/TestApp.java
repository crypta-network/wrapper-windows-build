package org.tanukisoftware.wrapper.testmodule.base;

import java.lang.Module;

public class TestApp
{
    private Thread m_runner;
    private boolean m_stopped = false;
    
    public void TestApp()
    {
    }    

    public void start( Module mainModule )
    {
        System.out.println( "Tanuki Software Test Application for integration with modules." );
        System.out.println( "  Main module: '" + mainModule.getName() + "' (calling " + TestApp.class.getModule() + ")" );
        System.out.println( "Running for 2 min... (press CTRL+C to cancel)" );

        m_runner = new Thread( "TestModule-Runner" )
        {
            public void run()
            {
                synchronized( m_runner )
                {
                    try
                    {
                        m_runner.wait( 120000 );
                        if (m_stopped)
                        {
                            System.err.println( "Wait interrupted" );
                        }
                        else
                        {
                            System.err.println( "Wait completed" );
                        }
                    } 
                    catch (InterruptedException e)
                    {
                        Thread.currentThread().interrupt(); 
                        System.err.println( "Thread interrupted" );
                    }
                }
            }
        };
        m_runner.start();
    }
    
    public void stop()
    {
        synchronized( m_runner )
        {
            m_stopped = true;
            m_runner.notify();
        }
    }
}
