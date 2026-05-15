#include <windows.h>
#include <sqltypes.h>
#include <sql.h>
#include <sqlext.h>
#include <iostream>
#include <fstream>

int main() {
    SQLHENV hEnv;
    SQLHDBC hDbc;
    SQLHSTMT hStmt;

    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv);
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc);

    SQLCHAR connOut[1024]; SQLSMALLINT cbConn;
    SQLRETURN ret = SQLDriverConnectA(hDbc, NULL, (SQLCHAR*)"DRIVER={SQL Server};SERVER=127.0.0.1;DATABASE=xiah;UID=sa;PWD=123456", SQL_NTS, connOut, sizeof(connOut), &cbConn, SQL_DRIVER_NOPROMPT);
    
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        std::ofstream out("db_test.log"); out << "Failed to connect to DB." << std::endl; return 1;
    }

    SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt);
    SQLExecDirectA(hStmt, (SQLCHAR*)"SELECT * FROM LOCATION", SQL_NTS);

    SQLSMALLINT columns = 0;
    SQLNumResultCols(hStmt, &columns);

    std::ofstream out("db_test.log");
    out << "Columns: " << columns << std::endl;
    for (int i = 1; i <= columns; ++i) {
        SQLCHAR colName[256];
        SQLSMALLINT nameLen, dataType, decimalDigits, nullable;
        SQLULEN columnSize;
        SQLDescribeColA(hStmt, i, colName, sizeof(colName), &nameLen, &dataType, &columnSize, &decimalDigits, &nullable);
        out << "Col " << i << ": " << colName << std::endl;
    }

    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    SQLDisconnect(hDbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    SQLFreeHandle(SQL_HANDLE_ENV, hEnv);

    return 0;
}
