#include "DBHelper.h"
#include "ServerCore.h"
#include <iostream>

DBHelper& DBHelper::GetInstance() {
    static DBHelper instance;
    return instance;
}

DBHelper::DBHelper() : hEnv(NULL), hDbc(NULL), isConnected(false) {
    connectionString = g_Config.GetConnectionString(g_Config.dbGame);
    Connect();
}

DBHelper::~DBHelper() {
    Disconnect();
}

bool DBHelper::Connect() {
    if (isConnected) return true;
    
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv))) return false;
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc))) {
        SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
        hEnv = NULL;
        return false;
    }
    
    SQLCHAR connOut[1024]; SQLSMALLINT cbConn;
    SQLRETURN ret = SQLDriverConnectA(hDbc, NULL, (SQLCHAR*)connectionString.c_str(), SQL_NTS, connOut, sizeof(connOut), &cbConn, SQL_DRIVER_NOPROMPT);
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        isConnected = true;
        return true;
    }
    
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
    hDbc = NULL; hEnv = NULL;
    return false;
}

void DBHelper::Disconnect() {
    if (hDbc) {
        SQLDisconnect(hDbc);
        SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
        hDbc = NULL;
    }
    if (hEnv) {
        SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
        hEnv = NULL;
    }
    isConnected = false;
}

bool DBHelper::ExecuteQuery(const std::string& query, std::function<void(SQLHSTMT)> rowCallback) {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!isConnected && !Connect()) return false;
    
    SQLHSTMT hStmt = NULL;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt))) return false;
    
    bool success = false;
    SQLRETURN ret = SQLExecDirectA(hStmt, (SQLCHAR*)query.c_str(), SQL_NTS);
    if (SQL_SUCCEEDED(ret)) {
        success = true;
        while (SQL_SUCCEEDED(SQLFetch(hStmt))) {
            rowCallback(hStmt);
        }
    } else {
        SQLCHAR sqlState[6], msg[SQL_MAX_MESSAGE_LENGTH];
        SQLINTEGER nativeError;
        SQLSMALLINT msgLen;
        SQLGetDiagRecA(SQL_HANDLE_STMT, hStmt, 1, sqlState, &nativeError, msg, sizeof(msg), &msgLen);
        LOG("[DBHelper] SQLExecDirectA Failed! Error: " + std::string((char*)msg));
    }
    
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    return success;
}

bool DBHelper::ExecuteUpdate(const std::string& query) {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (!isConnected && !Connect()) return false;
    
    SQLHSTMT hStmt = NULL;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt))) return false;
    
    SQLRETURN ret = SQLExecDirectA(hStmt, (SQLCHAR*)query.c_str(), SQL_NTS);
    // SQL_NO_DATA (100) means 0 rows affected - this is valid for DELETE/UPDATE
    bool success = SQL_SUCCEEDED(ret) || ret == SQL_NO_DATA;
    if (!success) {
        SQLCHAR sqlState[6] = {0};
        SQLCHAR msg[SQL_MAX_MESSAGE_LENGTH] = {0};
        SQLINTEGER nativeError;
        SQLSMALLINT msgLen = 0;
        SQLGetDiagRecA(SQL_HANDLE_STMT, hStmt, 1, sqlState, &nativeError, msg, sizeof(msg), &msgLen);
        LOG("[DBHelper] ExecuteUpdate Failed! Query: " + query + " Error: " + std::string((char*)msg, msgLen));
    }
    
    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
    return success;
}
