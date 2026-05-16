import pyodbc
conn = pyodbc.connect('DRIVER={SQL Server};SERVER=127.0.0.1;DATABASE=xiah;UID=sa;PWD=123456')
cursor = conn.cursor()
cursor.execute("SELECT szName, wMeleeAtkRangeInit, wShotAtkRangeInit FROM NPCTEMPLATE WHERE szName LIKE '%甲子%'")
for row in cursor:
    print(row)
