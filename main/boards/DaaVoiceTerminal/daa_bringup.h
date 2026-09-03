#ifndef DAA_BRINGUP_H
#define DAA_BRINGUP_H

// 按 CONFIG_DAA_BRINGUP_GATE 停在指定关。返回后调用方应死循环，不要上云。
void DaaBringupRun(int gate);

#endif
