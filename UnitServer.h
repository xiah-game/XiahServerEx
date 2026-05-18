#pragma once
#include <winsock2.h>
#include <windows.h>

// UnitServer: Network listen loop only. All business logic has been
// migrated to PlayerManager / ExpSystem modules.
void RunUnitSvr();
