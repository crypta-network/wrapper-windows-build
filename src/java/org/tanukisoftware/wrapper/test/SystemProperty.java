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
 * This test is to make sure that property values set in the wrapper config file
 *  are handled and passed into the JVM as expected.
 *
 * @author Tanuki Software Development Team &lt;support@tanukisoftware.com&gt;
 */
public class SystemProperty
{
    /* Parameters coming from the configuration file. */
    private static final String[][][] propsArray = {
        /* Test 1 */
        {
            { "prop1", "param1	with_tab" },
            { "prop2", "param2 with_space" },
            { "prop3", "\\\"param3 with_space" },
            { "prop4", "\\\"param4 with_space\\\"" },
            { "prop5", "\"param4 with_space\"" },
            { "prop6", "\"param4 with_space\"" },
            { "prop7", "\"param5 with_space\"" },
            { "prop8", "\"\\\"param6 with_space\"" },
            { "prop9", "\"\\\"param7 with_space\\\"\"" },
            { "prop10", "\\\"param8" },
            { "prop11", "\"\\\"param9\"" },
            { "prop12", "\\\"param10\\\"" },
            { "prop14", "\"\"" },
            { "prop15", "" },
            { "prop16", "  " },
            { "prop17", "\\" },
            { "prop18", "\\\\" },
            { "prop19", "\\\\\\" },
            { "prop20", "\\\\\\\\" },
            { "prop21", "\\" },
            { "prop22", "\\\"" },
            { "prop23", "\\\\\"" },
            { "prop24", "\\\\\\\"" },
            { "prop25", "\\\\\\\\\"" },
            { "prop26", "\\\"" },
            { "prop27", "\\-" },
            { "prop28", "\\\\-" },
            { "prop29", "\\\\\\-" },
            { "prop30", "\\\\\\\\-" },
            { "prop31", "\\-" },
            { "prop32", "param11 \\\\\\\\" },
            { "prop33", "param12\\\\\\\\" },
            { "prop34", "param13\\\\\\\"|||" },
            { "prop35", "param14\\\\\\\" |||" },
            { "prop36", "param15 \\\\\\\"|||" },
            { "prop37", "param16\\" },
            { "prop38", "param17" },
            { "prop39", "\"param18`\\ `\" `' \\' ' ''\"" },
            { "prop40", "param18`\\ `\" `' \\' ' ''" },
            { "prop41", "param19`\\ `\" `' \\' ' ''" },
            { "prop42", "param19`\\ `\" `' \\' ' ''" },
            { "prop43", "$A\\$B\\\\$C`$D" },
            { "prop44", "$A \\$B \\\\$C `$D" },
            { "prop45", "'$A\\$B\\\\$C`$D'" },
            { "prop46", "'$A \\$B \\\\$C `$D'" },
            { "prop47", "\"$A\\$B\\\\$C`$D\"" },
            { "prop48", "\"$A \\$B \\\\$C `$D\"" },
            { "prop49", "\"'$A\\$B\\\\$C`$D'\"" },
            { "prop50", "\"'$A \\$B \\\\$C `$D'\"" },
            { "prop51", "\"param20%$\"" },
            { "prop52", "param20%$" },
            { "prop53", "\"param21%$\"" },
            { "prop54", "param21%$" },
            { "prop55", "param22%\\\\\\\\ \\\\\"\"" },
            { "prop56", "param22%\\\\ \\" },
            { "prop57", "param23%\\\\\\\\ \\\\\\\"\"\"" },
            { "prop58", "param23%\\\\ \\\"" },
            { "prop59", "%\"'param24'\"%\\\\\\\\\"\" \\\\" },
            { "prop60", "%'param24'%\\\\\"\" \\" },
            { "prop61", "&" },
            { "prop62", "|" },
            { "prop63", ";" },
            { "prop64", "^" },
            { "prop65", "()" },
            { "prop66", "\"&" },
            { "prop67", "\"|" },
            { "prop68", "\";" },
            { "prop69", "\"^" },
            { "prop70", "\"()" },
            { "prop71", "\" &" },
            { "prop72", "\" |" },
            { "prop73", "\" ;" },
            { "prop74", "\" ^" },
            { "prop75", "\" ()" },
            { "prop76", "-AAA.BBB" },
            { "prop77", "-AAA.BBB$" },
            { "prop78", "-AAA.BBB\"" },
            { "prop79", "" },
            { "prop80", "#Arg" },
            { "prop81", "#Arg" },
            { "prop82", "-DA.B=C D" },
            { "prop83", "-D$A.B=C D" },
            { "prop84", "var with \"quotes\\\" and '\\' \\\\" },
            { "prop85", "var with \"quotes\\\" and '\\' \\\\" }
        },
        /* Test 2 */
        {
            { "prop1", "param1	with_tab" },
            { "prop2", "param2 with_space" },
            { "prop3", "\\\"param3 with_space" },
            { "prop4", "\\\"param4 with_space\\\"" },
            { "prop5", "\"param4 with_space\"" },
            { "prop6", "\"param4 with_space\"" },
            { "prop7", "\"param5 with_space\"" },
            { "prop8", "\"\\\"param6 with_space\"" },
            { "prop9", "\"\\\"param7 with_space\\\"\"" },
            { "prop10", "\\\"param8" },
            { "prop11", "\"\\\"param9\"" },
            { "prop12", "\\\"param10\\\"" },
            { "prop14", "\"\"" },
            { "prop15", "" },
            { "prop16", "  " },
            { "prop17", "\\" },
            { "prop18", "\\\\" },
            { "prop19", "\\\\\\" },
            { "prop20", "\\\\\\\\" },
            { "prop21", "\\" },
            { "prop22", "\\\"" },
            { "prop23", "\\\\\"" },
            { "prop24", "\\\\\\\"" },
            { "prop25", "\\\\\\\\\"" },
            { "prop26", "\\\"" },
            { "prop27", "\\-" },
            { "prop28", "\\\\-" },
            { "prop29", "\\\\\\-" },
            { "prop30", "\\\\\\\\-" },
            { "prop31", "\\-" },
            { "prop32", "param11 \\\\\\\\" },
            { "prop33", "param12\\\\\\\\" },
            { "prop34", "param13\\\\\\\"|||" },
            { "prop35", "param14\\\\\\\" |||" },
            { "prop36", "param15 \\\\\\\"|||" },
            { "prop37", "param16\\" },
            { "prop38", "param17" },
            { "prop39", "\"param18`\\ `\" `' \\' ' ''\"" },
            { "prop40", "param18`\\ `\" `' \\' ' ''" },
            { "prop41", "param19`\\ `\" `' \\' ' ''" },
            { "prop42", "param19`\\ `\" `' \\' ' ''" },
            { "prop43", "$A\\$B\\\\$C`$D" },
            { "prop44", "$A \\$B \\\\$C `$D" },
            { "prop45", "'$A\\$B\\\\$C`$D'" },
            { "prop46", "'$A \\$B \\\\$C `$D'" },
            { "prop47", "\"$A\\$B\\\\$C`$D\"" },
            { "prop48", "\"$A \\$B \\\\$C `$D\"" },
            { "prop49", "\"'$A\\$B\\\\$C`$D'\"" },
            { "prop50", "\"'$A \\$B \\\\$C `$D'\"" },
            { "prop51", "\"param20%$\"" },
            { "prop52", "param20%$" },
            { "prop53", "\"param21%$\"" },
            { "prop54", "param21%$" },
            { "prop55", "param22%\\\\\\\\ \\\\\"\"" },
            { "prop56", "param22%\\\\ \\" },
            { "prop57", "param23%\\\\\\\\ \\\\\\\"\"\"" },
            { "prop58", "param23%\\\\ \\\"" },
            { "prop59", "%\"'param24'\"%\\\\\\\\\"\" \\\\" },
            { "prop60", "%'param24'%\\\\\"\" \\" },
            { "prop61", "&" },
            { "prop62", "|" },
            { "prop63", ";" },
            { "prop64", "^" },
            { "prop65", "()" },
            { "prop66", "\"&" },
            { "prop67", "\"|" },
            { "prop68", "\";" },
            { "prop69", "\"^" },
            { "prop70", "\"()" },
            { "prop71", "\" &" },
            { "prop72", "\" |" },
            { "prop73", "\" ;" },
            { "prop74", "\" ^" },
            { "prop75", "\" ()" },
            { "prop76", "-AAA.BBB" },
            { "prop77", "-AAA.BBB$" },
            { "prop78", "-AAA.BBB\"" },
            { "prop79", "" },
            { "prop80", "#Arg" },
            { "prop81", "#Arg" },
            { "prop82", "-DA.B=C D" },
            { "prop83", "-D$A.B=C D" },
            { "prop84", "var with \"quotes\\\" and '\\' \\\\" },
            { "prop85", "var with \"quotes\\\" and '\\' \\\\" }
        },
        /* Test 3 */
        {
            { "prop1", "arg1" },
            { "prop2", "arg2" },
            { "prop3", "arg3" },
            { "prop4", "arg4" },
            { "prop5", "arg5" },
            { "prop6", "composed_arg6" },
            { "prop7", "[1:%FOO1%] [2:%FOO2%] [3:%FOO3%] [4:%FOO4%] [5:%FOO5%] [6:%FOO6%] [7:%FOO7%]" },
            { "prop8", "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%]" },
            { "prop9", "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%] [%BAR%]" },
            { "prop10", "[%dGVzdA==|base64%]" },
            { "prop11", "[%dGVzdA==|base64%] [%BAR%]" },
            { "prop12", "[%56Zv7Fouaui3w5V-eKx=YLoPW-G|r%] [%sSiofzoWh-Ga7-taDjWv|obf%]" },
            { "prop13", "[1:%] [2:%%] [3:%] [4:%] [5:%] [6:%FOO1%] [7:%FOO1%]" },
            { "prop14", "[%dGVzdA==|base64%]" },
            { "prop15", "[%dGVzdA==|base64%] [%BAR%]" },
            { "prop16", "[test]" },
            { "prop17", "[test] [%BAR%]" },
            { "prop18", "[hello] [%]" },
            { "prop19", "mypath=\"c:\\dir\"" },
            { "prop20", "mypath=\"c:\\dir\"" },
            { "prop21", "\u2BA4" },
            { "prop22", "\u2BA4" },
            { "file_prop1", "[1:%FOO1%] [2:%FOO2%] [3:%FOO3%] [4:%FOO4%] [5:%FOO5%] [6:%FOO6%] [7:%FOO7%]" },
            { "file_prop2", "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%]" },
            { "file_prop3", "[%WRAPPER_PERCENTAGE%dGVzdA==|base64%WRAPPER_PERCENTAGE%] [%BAR%]" },
            { "file_prop4", "[%dGVzdA==|base64%]" },
            { "file_prop5", "[%dGVzdA==|base64%] [%BAR%]" },
            { "file_prop6", "[%56Zv7Fouaui3w5V-eKx=YLoPW-G|r%] [%sSiofzoWh-Ga7-taDjWv|obf%]" },
            { "file_prop7", "[1:%] [2:%%] [3:%] [4:%] [5:%] [6:%FOO1%] [7:%FOO1%]" },
            { "file_prop8", "[%dGVzdA==|base64%]" },
            { "file_prop9", "[%dGVzdA==|base64%] [%BAR%]" },
            { "file_prop10", "[test]" },
            { "file_prop11", "[test] [%BAR%]" },
            { "file_prop12", "[hello] [%]" }
        },
        /* Test 4 */
        {
        },
        /* Test 5 */
        {
            { "prop1", "ABC" },
            { "prop2", "DEF" },
            { "prop3", "GHI" }
        },
        /* Test 6 */
        {
            { "prop", "\"	\\\\Hello \\\"World\\\"! \"" }
        },
        /* Test 7 */
        {
            { "prop", "	\\Hello \"World\"! " }
        },
        /* Test 8 */
        {
            { "prop", "ABC" }
        },
        /* Test 9 */
        {
            { "prop1", "param1" },
            { "prop2", "#param2" },
            { "prop3", "#param3" },
            { "prop4", "\"#param4\"" },
            { "prop5", "\"##param5\"" },
            { "prop6", "param6" },
            { "prop7", "param7	prop8=param8" },
            { "prop9", "param9 prop10=param10" },
            { "prop11", "\"param11	with_tab\"" },
            { "prop12", "\"param12 with_space\"" },
            { "prop13", "\"param\\\\_\\\\13'	\\\"with_tab\\\"\"   prop14=\"param14\\\\	`with``_``tab\"" },
            // prop15 should be skipped because invalid
            { "prop17", "\"param17`\\ `\" `' \\' ' ''\"" },
            { "prop18", "param18`\\ `\" `' \\' ' ''" },
            { "prop19", "\"\"" },
            { "prop20", "\\\"" },
            { "prop21", "\\\\" },
            { "prop22", "param22\\" },
            { "prop23", "var with \"quotes\\\" and '\\' \\\\" },
            { "prop24", "mypath=\"c:\\dir\"" }
        },
        /* Test 10 */
        {
            { "prop1", "param1" },
            { "prop2", "#param2" },
            { "prop3", "#param3" },
            { "prop4", "#param4" },
            { "prop5", "##param5" },
            { "prop6", "param6" },
            { "prop7", "param7" },
            { "prop8", "param8" },
            { "prop9", "param9" },
            { "prop10", "param10" },
            { "prop11", "param11	with_tab" },
            { "prop12", "param12 with_space" },
            { "prop13", "param\\_\\13'	\"with_tab\"" },
            { "prop14", "param14\\	`with``_``tab" },
            { "prop15", "param\\_\\15'	\"with_tab\"" },
            { "prop16", "param16\\	`with``_``tab  " },
            { "prop17", "param17`\\ `\" `' \\' ' ''" },
            { "prop18", "" },
            { "prop19", "\"" },
            { "prop20", "\\" },
            { "prop21", "param21\\" },
            { "prop22", "var with \"quotes\\\" and '\\' \\\\" },
            { "prop23", "mypath=\"c:\\dir\"" }
        },
        /* Test 11 */
        {
            { "prop1", "en" },
            { "prop2", System.getProperty( "user.dir" ) },
            { "prop3", getJvmBits() },
            { "prop4", "%NOT_A_VAR%" },
            { "prop5", "%WRAPPER_LANG%" },
            { "prop6", "%WRAPPER_BIN_DIR%" },
            { "prop7", "%WRAPPER_BITS%" },
            { "prop8", "%WRAPPER_PERCENTAGE%NOT_A_VAR%WRAPPER_PERCENTAGE%" }
        },
        /* Test 12 */
        {
        },
        /* Test 13 */
        {
            { "param1", "ABC" },
            { "param2", "DEF" },
            { "param3", "GHI" }
        },
        /* Test 14 */
        {
            { "param", "\"	\\\\Hello \\\"World\\\"! \"" }
        },
        /* Test 15 */
        {
            { "param", "	\\Hello \"World\"! " }
        },
        /* Test 16 */
        {
            { "param", "ABC" }
        },
        /* Test 17 */
        {
            { "param1", "param1" },
            { "param2", "#param2" },
            { "param3", "#param3" },
            { "param4", "\"#param4\"" },
            { "param5", "\"##param5\"" },
            { "param6", "param6" },
            { "param7", "param7	-Dparam8=param8" },
            { "param9", "param9 -Dparam10=param10" },
            { "param11", "\"param11	with_tab\"" },
            { "param12", "\"param12 with_space\"" },
            { "param13", "\"param\\\\_\\\\13'	\\\"with_tab\\\"\"   -Dparam14=\"param14\\\\	`with``_``tab\"" },
            // param15 should be skipped because invalid
            { "param17", "\"param17`\\ `\" `' \\' ' ''\"" },
            { "param18", "param18`\\ `\" `' \\' ' ''" },
            { "param19", "\"\"" },
            { "param20", "\\\"" },
            { "param21", "\\\\" },
            { "param22", "param22\\" },
            { "param23", "var with \"quotes\\\" and '\\' \\\\" }
        },
        /* Test 18 */
        {
            { "param1", "param1" },
            { "param2", "#param2" },
            { "param3", "#param3" },
            { "param4", "#param4" },
            { "param5", "##param5" },
            { "param6", "param6" },
            { "param7", "param7" },
            { "param8", "param8" },
            { "param9", "param9" },
            { "param10", "param10" },
            { "param11", "param11	with_tab" },
            { "param12", "param12 with_space" },
            { "param13", "param\\_\\13'	\"with_tab\"" },
            { "param14", "param14\\	`with``_``tab" },
            { "param15", "param\\_\\15'	\"with_tab\"" },
            { "param16", "param16\\	`with``_``tab  " },
            { "param17", "param17`\\ `\" `' \\' ' ''" },
            { "param18", "" },
            { "param19", "\"" },
            { "param20", "\\" },
            { "param21", "param21\\" },
            { "param22", "var with \"quotes\\\" and '\\' \\\\" }
        },
        /* Test 19 */
        {
            { "param1", "en" },
            { "param2", System.getProperty( "user.dir" ) },
            { "param3", getJvmBits() },
            { "param4", "%NOT_A_VAR%" },
            { "param5", "%WRAPPER_LANG%" },
            { "param6", "%WRAPPER_BIN_DIR%" },
            { "param7", "%WRAPPER_BITS%" },
            { "param8", "%WRAPPER_PERCENTAGE%NOT_A_VAR%WRAPPER_PERCENTAGE%" }
        }
    };


    /*---------------------------------------------------------------
     * Main Method
     *-------------------------------------------------------------*/
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

        if ( ( index >= 0 ) && ( index < propsArray.length ) )
        {
            int len = propsArray[index].length;

            String[] names = new String[len];
            for ( int i = 0; i < len; i++ )
            {
                names[i] = propsArray[index][i][0];
            }

            String[] expectedValues = new String[len];
            for ( int i = 0; i < len; i++ )
            {
                expectedValues[i] = propsArray[index][i][1];
            }

            DecimalFormat df = new DecimalFormat( "00" );
            for ( int i = 0; i < len; i++ )
            {
                String name = names[i];
                String expectedValue = expectedValues[i];
                String value = System.getProperty( name );
                String formattedNumber = df.format( i );
                if ( expectedValue.equals( value ) )
                {
                    System.out.println( formattedNumber + ") [OK    ] [" + name + "=" + value + "]" );
                }
                else
                {
                    System.out.println( formattedNumber + ") [FAILED] [" + expectedValue + "] != [" + value + "]" );
                }
            }
        }
        else
        {
            System.out.println( "Invalid index!" );
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
