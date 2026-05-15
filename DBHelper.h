#pragma once
#include <winsock2.h>
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <string>
#include <functional>
#include <vector>
#include <mutex>

class DBHelper {
public:
    static DBHelper& GetInstance();
    
    // Executes a query and calls the callback for each row fetched
    bool ExecuteQuery(const std::string& query, std::function<void(SQLHSTMT)> rowCallback);
    
    // Executes a direct SQL statement (e.g., INSERT/UPDATE/DELETE)
    bool ExecuteUpdate(const std::string& query);

private:
    DBHelper();
    ~DBHelper();
    
    SQLHENV hEnv;
    SQLHDBC hDbc;
    bool isConnected;
    std::string connectionString;
    std::mutex dbMutex;
    
    bool Connect();
    void Disconnect();
};
