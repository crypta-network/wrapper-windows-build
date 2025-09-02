package org.tanukisoftware.wrapper.test;

import java.text.DecimalFormat;
import org.tanukisoftware.wrapper.WrapperSystemPropertyUtil;

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

/**
 * This is a very simple test if backend parameters are received correctly.
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class Parameters
{
    /* Parameters coming from the configuration file. */
    private static final String[][] paramsArray = {
        /* Test 1 */
        {
            "param1	with_tab",
            "param2 with_space",
            "\\\"param3 with_space",
            "\\\"param4 with_space\\\"",
            "\"param4 with_space\"",
            "\"param4 with_space\"",
            "\"param5 with_space\"",
            "\"\\\"param6 with_space\"",
            "\"\\\"param7 with_space\\\"\"",
            "\\\"param8",
            "\"\\\"param9\"",
            "\\\"param10\\\"",
            "",
            "\"\"",
            "",
            "  ",
            "\\",
            "\\\\",
            "\\\\\\",
            "\\\\\\\\",
            "\\",
            "\\\"",
            "\\\\\"",
            "\\\\\\\"",
            "\\\\\\\\\"",
            "\\\"",
            "\\-",
            "\\\\-",
            "\\\\\\-",
            "\\\\\\\\-",
            "\\-",
            "param11 \\\\\\\\",
            "param12\\\\\\\\",
            "param13\\\\\\\"|||",
            "param14\\\\\\\" |||",
            "param15 \\\\\\\"|||",
            "param16\\",
            "param17",
            "\"param18`\\ `\" `' \\' ' ''\"",
            "param18`\\ `\" `' \\' ' ''",
            "param19`\\ `\" `' \\' ' ''",
            "param19`\\ `\" `' \\' ' ''",
            "$A\\$B\\\\$C`$D",
            "$A \\$B \\\\$C `$D",
            "'$A\\$B\\\\$C`$D'",
            "'$A \\$B \\\\$C `$D'",
            "\"$A\\$B\\\\$C`$D\"",
            "\"$A \\$B \\\\$C `$D\"",
            "\"'$A\\$B\\\\$C`$D'\"",
            "\"'$A \\$B \\\\$C `$D'\"",
            "\"param20%$\"",
            "param20%$",
            "\"param21%$\"",
            "param21%$",
            "param22%\\\\\\\\ \\\\\"\"",
            "param22%\\\\ \\",
            "param23%\\\\\\\\ \\\\\\\"\"\"",
            "param23%\\\\ \\\"",
            "%\"'param24'\"%\\\\\\\\\"\" \\\\",
            "%'param24'%\\\\\"\" \\",
            "&",
            "|",
            ";",
            "^",
            "()",
            "\"&",
            "\"|",
            "\";",
            "\"^",
            "\"()",
            "\" &",
            "\" |",
            "\" ;",
            "\" ^",
            "\" ()",
            "-AAA.BBB",
            "-AAA.BBB$",
            "-AAA.BBB\"",
            "",
            "#Arg",
            "#Arg",
            "--%",
            "-DA.B=C D",
            "-D$A.B=C D",
            "var with \"quotes\\\" and '\\' \\\\",
            "var with \"quotes\\\" and '\\' \\\\"
        },
        /* Test 2 */
        {
            "arg1",
            "arg2",
            "arg3",
            "arg4",
            "arg5",
            "composed_arg6",
            "---",
            "[1:%FOO1%] [2:%FOO2%] [3:%FOO3%] [4:%FOO4%] [5:%FOO5%] [6:%FOO6%] [7:%FOO7%]",
            "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%]",
            "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%] [%BAR%]",
            "[%dGVzdA==|base64%]",
            "[%dGVzdA==|base64%] [%BAR%]",
            "[%56Zv7Fouaui3w5V-eKx=YLoPW-G|r%] [%sSiofzoWh-Ga7-taDjWv|obf%]",
            "---",
            "[1:%] [2:%%] [3:%] [4:%] [5:%] [6:%FOO1%] [7:%FOO1%]",
            "[%dGVzdA==|base64%]",
            "[%dGVzdA==|base64%] [%BAR%]",
            "[test]",
            "[test] [%BAR%]",
            "[hello] [%]",
            "---",
            "mypath=\"c:\\dir\"",
            "mypath=\"c:\\dir\"",
            "---",
            "\u2BA4",
            "\u2BA4",
            "---",
            "[1:%FOO1%] [2:%FOO2%] [3:%FOO3%] [4:%FOO4%] [5:%FOO5%] [6:%FOO6%] [7:%FOO7%]",
            "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%]",
            "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%] [%BAR%]",
            "[%dGVzdA==|base64%]",
            "[%dGVzdA==|base64%] [%BAR%]",
            "[%56Zv7Fouaui3w5V-eKx=YLoPW-G|r%] [%sSiofzoWh-Ga7-taDjWv|obf%]",
            "---",
            "[1:%] [2:%%] [3:%] [4:%] [5:%] [6:%FOO1%] [7:%FOO1%]",
            "[%dGVzdA==|base64%]",
            "[%dGVzdA==|base64%] [%BAR%]",
            "[test]",
            "[test] [%BAR%]",
            "[hello] [%]"
        },
        /* Test 3 */
        {
        },
        /* Test 4 */
        {
            "ABC",
            "DEF",
            "GHI"
        },
        /* Test 5 */
        {
            "\"	\\\\Hello \\\"World\\\"! \""
        },
        /* Test 6 */
        {
            "	\\Hello \"World\"! "
        },
        /* Test 7 */
        {
            "ABC"
        },
        /* Test 8 */
        {
            "ABC",
            "\\",
            "D#EF",
            "\"#GHI\"",
            "\"##JKL\"",
            "# LMN",
            "@ OPQ",
            "@ OPQ",
            "param1",
            "param2	param3",
            "param4 param5",
            "\"param6	with_tab\"",
            "\"param7 with_space\"",
            "\"param\\\\_\\\\8'	\\\"with_tab\\\"\"   \"param9\\\\	`with``_``tab\"",
            "\"param10`\\ `\" `' \\' ' ''\"",
            "param11`\\ `\" `' \\' ' ''",
            "\"\"",
            "\\\"",
            "\\\\",
            "param12\\",
            "var with \"quotes\\\" and '\\' \\\\",
            "mypath=\"c:\\dir\""
,        },
        /* Test 9 */
        {
            "ABC",
            "\\",
            "D#EF",
            "#GHI",
            "##JKL",
            "#",
            "LMN",
            "@",
            "OPQ",
            "@",
            "OPQ",
            "param1",
            "param2",
            "param3",
            "param4",
            "param5",
            "param6	with_tab",
            "param7 with_space",
            "param\\_\\8'	\"with_tab\"",
            "param9\\	`with``_``tab",
            "param10`\\ `\" `' \\' ' ''",
            "",
            "\"",
            "\\",
            "param11\\",
            "var with \"quotes\\\" and '\\' \\\\",
            "mypath=\"c:\\dir\""
        },
        /* Test 10 */
        {
            "en",
            System.getProperty( "user.dir" ),
            getJvmBits(),
            "%NOT_A_VAR%",
            "%WRAPPER_LANG%",
            "%WRAPPER_BIN_DIR%",
            "%WRAPPER_BITS%",
            "%WRAPPER_PERCENTAGE%NOT_A_VAR%WRAPPER_PERCENTAGE%"
        }
    };

    public static void main(String[] args)
    {
        // First argument is the Test number (index to find the corresponding arrays of parameters). */
        int index;
        try
        {
            index = args.length > 0 ? (Integer.parseInt( args[0] ) - 1) : -1;
            System.out.println( "Running test number " + ( index + 1 ) );
        }
        catch ( NumberFormatException e )
        {
            index = -1;
        }

        if ( ( index >= 0 ) && ( index < paramsArray.length ) )
        {
            int len = paramsArray[index].length;
            String[] params = new String[len];

            System.arraycopy(paramsArray[index], 0, params, 0, len);

            if ( len != args.length - 1 )
            {
                System.out.println( "Expected " + ( len + 1 ) + " parameter(s), received " + args.length + "." );
                if ( args.length - 1 < len )
                {
                    len = args.length - 1;
                }
            }
            
            DecimalFormat df = new DecimalFormat("00");
            for ( int i = 0; i < len; i++ )
            {
                String param = params[i];
                String formattedNumber = df.format( i );
                if ( param.equals( args[i + 1] ) )
                {
                    System.out.println( formattedNumber + ") [OK    ] [" + param + "]" );
                }
                else
                {
                    System.out.println( formattedNumber + ") [FAILED] [" + param + "] != [" + args[i + 1] + "]" );
                }
            }
        }
        else
        {
            System.out.println("Invalid index!" );
        }
    }

    private static String getJvmBits() {
        int jvmBits = WrapperSystemPropertyUtil.getIntProperty( "sun.arch.data.model", -1 );
        if ( jvmBits == -1 )
        {
            jvmBits = WrapperSystemPropertyUtil.getIntProperty( "com.ibm.vm.bitmode", -1 );
        }
        return String.valueOf( jvmBits );
    }
}
