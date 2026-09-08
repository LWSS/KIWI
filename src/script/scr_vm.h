#pragma once
#include "scr_variable.h"

#include <xanim/xanim.h>

#include "scr_debugger.h"
#include <bgame/bg_local.h>

enum $3FAD84344DD9017EDEA6C2E0F6A382A4 : int
{
    SCR_SYS_GAME = 0x1,
};

// LWSS: Custom named enum so I'm forced to use this on EmitOpcode()
enum Opcode_t : int
{
    OP_End = 0x0,
    OP_Return = 0x1,
    OP_GetUndefined = 0x2,
    OP_GetZero = 0x3,
    OP_GetByte = 0x4,
    OP_GetNegByte = 0x5,
    OP_GetUnsignedShort = 0x6,
    OP_GetNegUnsignedShort = 0x7,
    OP_GetInteger = 0x8,
    OP_GetFloat = 0x9,
    OP_GetString = 0xA,
    OP_GetIString = 0xB,
    OP_GetVector = 0xC,
    OP_GetLevelObject = 0xD,
    OP_GetAnimObject = 0xE,
    OP_GetSelf = 0xF,
    OP_GetLevel = 0x10,
    OP_GetGame = 0x11,
    OP_GetAnim = 0x12,
    OP_GetAnimation = 0x13,
    OP_GetGameRef = 0x14,
    OP_GetFunction = 0x15,
    OP_CreateLocalVariable = 0x16,
    OP_RemoveLocalVariables = 0x17,
    OP_EvalLocalVariableCached0 = 0x18,
    OP_EvalLocalVariableCached1 = 0x19,
    OP_EvalLocalVariableCached2 = 0x1A,
    OP_EvalLocalVariableCached3 = 0x1B,
    OP_EvalLocalVariableCached4 = 0x1C,
    OP_EvalLocalVariableCached5 = 0x1D,
    OP_EvalLocalVariableCached = 0x1E,
    OP_EvalLocalArrayCached = 0x1F,
    OP_EvalArray = 0x20,
    OP_EvalLocalArrayRefCached0 = 0x21,
    OP_EvalLocalArrayRefCached = 0x22,
    OP_EvalArrayRef = 0x23,
    OP_ClearArray = 0x24,
    OP_EmptyArray = 0x25,
    OP_GetSelfObject = 0x26,
    OP_EvalLevelFieldVariable = 0x27,
    OP_EvalAnimFieldVariable = 0x28,
    OP_EvalSelfFieldVariable = 0x29,
    OP_EvalFieldVariable = 0x2A,
    OP_EvalLevelFieldVariableRef = 0x2B,
    OP_EvalAnimFieldVariableRef = 0x2C,
    OP_EvalSelfFieldVariableRef = 0x2D,
    OP_EvalFieldVariableRef = 0x2E,
    OP_ClearFieldVariable = 0x2F,
    OP_SafeCreateVariableFieldCached = 0x30,
    OP_SafeSetVariableFieldCached0 = 0x31,
    OP_SafeSetVariableFieldCached = 0x32,
    OP_SafeSetWaittillVariableFieldCached = 0x33,
    OP_clearparams = 0x34,
    OP_checkclearparams = 0x35,
    OP_EvalLocalVariableRefCached0 = 0x36,
    OP_EvalLocalVariableRefCached = 0x37,
    OP_SetLevelFieldVariableField = 0x38,
    OP_SetVariableField = 0x39,
    OP_SetAnimFieldVariableField = 0x3A,
    OP_SetSelfFieldVariableField = 0x3B,
    OP_SetLocalVariableFieldCached0 = 0x3C,
    OP_SetLocalVariableFieldCached = 0x3D,
    OP_CallBuiltin0 = 0x3E,
    OP_CallBuiltin1 = 0x3F,
    OP_CallBuiltin2 = 0x40,
    OP_CallBuiltin3 = 0x41,
    OP_CallBuiltin4 = 0x42,
    OP_CallBuiltin5 = 0x43,
    OP_CallBuiltin = 0x44,
    OP_CallBuiltinMethod0 = 0x45,
    OP_CallBuiltinMethod1 = 0x46,
    OP_CallBuiltinMethod2 = 0x47,
    OP_CallBuiltinMethod3 = 0x48,
    OP_CallBuiltinMethod4 = 0x49,
    OP_CallBuiltinMethod5 = 0x4A,
    OP_CallBuiltinMethod = 0x4B,
    OP_wait = 0x4C,
    OP_waittillFrameEnd = 0x4D,
    OP_PreScriptCall = 0x4E,
    OP_ScriptFunctionCall2 = 0x4F,
    OP_ScriptFunctionCall = 0x50,
    OP_ScriptFunctionCallPointer = 0x51,
    OP_ScriptMethodCall = 0x52,
    OP_ScriptMethodCallPointer = 0x53,
    OP_ScriptThreadCall = 0x54,
    OP_ScriptThreadCallPointer = 0x55,
    OP_ScriptMethodThreadCall = 0x56,
    OP_ScriptMethodThreadCallPointer = 0x57,
    OP_DecTop = 0x58,
    OP_CastFieldObject = 0x59,
    OP_EvalLocalVariableObjectCached = 0x5A,
    OP_CastBool = 0x5B,
    OP_BoolNot = 0x5C,
    OP_BoolComplement = 0x5D,
    OP_JumpOnFalse = 0x5E,
    OP_JumpOnTrue = 0x5F,
    OP_JumpOnFalseExpr = 0x60,
    OP_JumpOnTrueExpr = 0x61,
    OP_jump = 0x62,
    OP_jumpback = 0x63,
    OP_inc = 0x64,
    OP_dec = 0x65,
    OP_bit_or = 0x66,
    OP_bit_ex_or = 0x67,
    OP_bit_and = 0x68,
    OP_equality = 0x69,
    OP_inequality = 0x6A,
    OP_less = 0x6B,
    OP_greater = 0x6C,
    OP_less_equal = 0x6D,
    OP_greater_equal = 0x6E,
    OP_shift_left = 0x6F,
    OP_shift_right = 0x70,
    OP_plus = 0x71,
    OP_minus = 0x72,
    OP_multiply = 0x73,
    OP_divide = 0x74,
    OP_mod = 0x75,
    OP_size = 0x76,
    OP_waittillmatch = 0x77,
    OP_waittill = 0x78,
    OP_notify = 0x79,
    OP_endon = 0x7A,
    OP_voidCodepos = 0x7B,
    OP_switch = 0x7C,
    OP_endswitch = 0x7D,
    OP_vector = 0x7E,
    OP_NOP = 0x7F,
    OP_abort = 0x80,
    OP_object = 0x81,
    OP_thread_object = 0x82,
    OP_EvalLocalVariable = 0x83,
    OP_EvalLocalVariableRef = 0x84,
    OP_prof_begin = 0x85,
    OP_prof_end = 0x86,
    OP_breakpoint = 0x87,
    OP_assignmentBreakpoint = 0x88,
    OP_manualAndAssignmentBreakpoint = 0x89,
    OP_count = 0x8A,
};
inline Opcode_t &operator++(Opcode_t &e) {
    e = static_cast<Opcode_t>(static_cast<int>(e) + 1);
    return e;
}
inline Opcode_t &operator++(Opcode_t &e, int i)
{
    ++e;
    return e;
}

struct Scr_StringNode_s // sizeof=0x8
{
    const char *text;
    Scr_StringNode_s *next;
};
static_assert(sizeof(Scr_StringNode_s) == (sizeof(void *) == 8 ? 16 : 0x8));

struct function_stack_t // sizeof=0x14
{                                       // ...
    const char *pos;                    // ...
    uint localId;               // ...
    uint localVarCount;         // ...
    VariableValue *top;                 // ...
    VariableValue *startTop;            // ...
};
static_assert(sizeof(function_stack_t) == (sizeof(void *) == 8 ? 32 : 0x14));

struct function_frame_t // sizeof=0x18
{                                       // ...
    function_stack_t fs;                // ...
    Vartype_t topType;
};
static_assert(sizeof(function_frame_t) == (sizeof(void *) == 8 ? 40 : 0x18));

struct scrVmPub_t // sizeof=0x4328
{                                       // ...
    uint* localVars;            // ...
    VariableValue* maxstack;            // ...
    int function_count;                 // ...
    function_frame_t* function_frame;   // ...
    VariableValue* top;                 // ...
    bool debugCode;                     // ...
    bool abort_on_error;                // ...
    bool terminal_error;                // ...
    // padding byte
    uint inparamcount;          // ...
    uint outparamcount;         // ...
    uint breakpointOutparamcount; // ...
    bool showError;                     // ...
    // padding byte
    // padding byte
    // padding byte
    function_frame_t function_frame_start[32]; // ...
    VariableValue stack[2048];          // ...
};
static_assert(sizeof(scrVmPub_t) == (sizeof(void *) == 8 ? 34112 : 0x4328));

struct FuncDebugData // sizeof=0x10
{                                       // ...
    int breakpointCount;                // ...
    const char *name;                   // ...
    int prof;                           // ...
    int usage;                          // ...
};
static_assert(sizeof(FuncDebugData) == (sizeof(void *) == 8 ? 24 : 0x10));

struct scrVmDebugPub_t // sizeof=0x24210
{                                       // ...
    FuncDebugData func_table[1024];     // ...
    int checkBreakon;                   // ...
    int profileEnable[32768];           // ...
    int builtInTime;                    // ...
    const char *jumpbackHistory[128];   // ...
    int jumpbackHistoryIndex;           // ...
    int dummy;
};
static_assert(sizeof(scrVmDebugPub_t) == (sizeof(void *) == 8 ? 156688 : 0x24210));

struct scrVmGlob_t // sizeof=0x2028
{                                       // ...
    VariableValue eval_stack[2];        // ...
    const char *dialog_error_message;   // ...
    int loading;                        // ...
    int starttime;                      // ...
    uint localVarsStack[2048];  // ...
    bool recordPlace;                   // ...
    // padding byte
    // padding byte
    // padding byte
    char *lastFileName;                 // ...
    int lastLine;                       // ...
};
static_assert(sizeof(scrVmGlob_t) == (sizeof(void *) == 8 ? 8264 : 0x2028));

void Scr_Error(const char* error);
void Scr_ErrorWithDialogMessage(const char *error, const char *dialog_error);

void __cdecl SCR_Init();
void GScr_GetAnimLength();
void __cdecl Scr_ErrorOnDefaultAsset(XAssetType type, const char* assetName);
void(__cdecl* __cdecl Scr_GetFunction(const char** pName, int* type))();
uint Scr_GetFunc(uint index);
void Scr_SetRecordScriptPlace(int on);
void Scr_GetLastScriptPlace(int *line, const char **filename);
struct XAnim_s *Scr_GetAnimTree(uint index);
void(__cdecl *__cdecl Scr_GetMethod(const char **pName, int *type))(scr_entref_t);
void(__cdecl *__cdecl BuiltIn_GetMethod(const char **pName, int *type))(scr_entref_t);
void __cdecl GScr_AddVector(const float* vVec);
void __cdecl GScr_Shutdown();
void __cdecl GScr_SetDynamicEntityField(gentity_s* ent, uint index);
void __cdecl Scr_InitFromChildBlocks(struct scr_block_s** childBlocks, int childCount, struct scr_block_s* block);
Scr_StringNode_s* __cdecl Scr_GetStringList(const char* filename, char** pBuf);
void __cdecl Scr_SetSelectionComp(struct UI_Component *comp);
void __cdecl Scr_InitDebuggerSystem();
void Scr_InitBreakpoints();
void __cdecl Scr_ShutdownDebuggerSystem(int restart);
void __cdecl Scr_ShutdownRemoteClient(int restart);
int __cdecl Scr_GetFunctionHandle(const char* filename, const char* name);
int __cdecl Scr_GetStringUsage();
void __cdecl Scr_ShutdownGameStrings();
void __cdecl TRACK_scr_vm();
void __cdecl Scr_ClearErrorMessage();
void __cdecl Scr_Init();
const dvar_s* Scr_VM_Init();
void __cdecl Scr_Settings(int developer, int developer_script, int abort_on_error);
void __cdecl Scr_Shutdown();
void VM_Shutdown();
void __cdecl Scr_SetLoading(int bLoading);
uint __cdecl Scr_GetNumScriptThreads();
void __cdecl Scr_ClearOutParams();
char* __cdecl Scr_GetReturnPos(uint* localId);
char* __cdecl Scr_GetNextCodepos(VariableValue* top, const char* pos, int opcode, int mode, uint* localId);
void __cdecl VM_CancelNotify(uint notifyListOwnerId, uint startLocalId);
void __cdecl VM_CancelNotifyInternal(
    uint notifyListOwnerId,
    uint startLocalId,
    uint notifyListId,
    uint notifyNameListId,
    uint stringValue);
bool __cdecl Scr_IsEndonThread(uint localId);
uint __cdecl Scr_GetWaittillThreadStackId(uint localId, uint startLocalId);
const char* __cdecl Scr_GetThreadPos(uint localId);
const char* __cdecl Scr_GetStackThreadPos(uint endLocalId, VariableStackBuffer* stackValue, bool killThread);
const char* __cdecl Scr_GetRunningThreadPos(uint localId);
uint __cdecl Scr_GetWaitThreadStackId(uint localId, uint startLocalId);
void __cdecl Scr_NotifyNum(
    uint entnum,
    uint classnum,
    uint stringValue,
    uint paramcount);
void __cdecl VM_Notify(uint notifyListOwnerId, uint stringValue, VariableValue* top);
void __cdecl Scr_TerminateThread(uint localId);
void __cdecl Scr_TerminateRunningThread(uint localId);
void __cdecl Scr_TerminateWaitThread(uint localId, uint startLocalId);
void __cdecl VM_TerminateStack(uint endLocalId, uint startLocalId, VariableStackBuffer* stackValue);
void __cdecl Scr_TerminateWaittillThread(uint localId, uint startLocalId);
void __cdecl Scr_CancelNotifyList(uint notifyListOwnerId);
void __cdecl VM_TrimStack(uint startLocalId, VariableStackBuffer* stackValue, bool fromEndon);
void __cdecl Scr_CancelWaittill(uint startLocalId);
uint16_t __cdecl Scr_ExecThread(int handle, uint paramcount);
uint __cdecl VM_Execute(uint localId, const char* pos, uint paramcount);
//uint __cdecl VM_Execute_0();
uint __cdecl GetDummyObject();
uint __cdecl GetDummyFieldValue();
void VM_PrintJumpHistory();
VariableStackBuffer* __cdecl VM_ArchiveStack();
uint16_t __cdecl Scr_ExecEntThreadNum(
    uint entnum,
    uint classnum,
    int handle,
    uint paramcount);
void __cdecl Scr_AddExecThread(int handle, uint paramcount);
void __cdecl Scr_FreeThread(uint16_t handle);
void __cdecl Scr_ExecCode(const char* pos, uint localId);
void __cdecl Scr_InitSystem(int sys);
void __cdecl Scr_ShutdownSystem(uint8_t sys, int bComplete);
void __cdecl VM_TerminateTime(uint timeId);
BOOL __cdecl Scr_IsSystemActive(); // LWSS: Note this has a "system" argument, however it's not used and optimized out in some builds
int __cdecl Scr_GetInt(uint index);
scr_anim_s __cdecl Scr_GetAnim(uint index, XAnimTree_s* tree);
BOOL Scr_ErrorInternal();
float __cdecl Scr_GetFloat(uint index);
uint __cdecl Scr_GetConstString(uint index);
uint __cdecl Scr_GetConstLowercaseString(uint index);
const char* __cdecl Scr_GetString(uint index);
uint __cdecl Scr_GetConstStringIncludeNull(uint index);
const char* __cdecl Scr_GetDebugString(uint index);
uint __cdecl Scr_GetConstIString(uint index);
const char* __cdecl Scr_GetIString(uint index);
void __cdecl Scr_GetVector(uint index, float* vectorValue);
scr_entref_t __cdecl Scr_GetEntityRef(uint index);
uint __cdecl Scr_GetObject(uint index);
int __cdecl Scr_GetType(uint index);
const char* __cdecl Scr_GetTypeName(uint index);
uint __cdecl Scr_GetPointerType(uint index);
uint __cdecl Scr_GetNumParam();
void __cdecl Scr_AddBool(uint value);
void IncInParam();
void __cdecl Scr_AddInt(int value);
void __cdecl Scr_AddFloat(float value);
void __cdecl Scr_AddAnim(scr_anim_s value);
void __cdecl Scr_AddUndefined();
void __cdecl Scr_AddObject(uint id);
void __cdecl Scr_AddEntityNum(uint entnum, uint classnum);
void __cdecl Scr_AddStruct();
void __cdecl Scr_AddString(const char* value);
void __cdecl Scr_AddIString(const char* value);
void __cdecl Scr_AddConstString(uint value);
void __cdecl Scr_AddVector(const float* value);
void __cdecl Scr_MakeArray();
void __cdecl Scr_AddArray();
void __cdecl Scr_AddArrayStringIndexed(uint stringValue);
void __cdecl Scr_Error(const char* error);
void __cdecl Scr_SetErrorMessage(const char* error);
void __cdecl Scr_TerminalError(const char* error);
void __cdecl Scr_NeverTerminalError(const char* error);
void __cdecl Scr_ParamError(uint index, const char* error);
void __cdecl Scr_ObjectError(const char* error);
char __cdecl SetEntityFieldValue(uint classnum, int entnum, int offset, VariableValue* value);
VariableValue __cdecl GetEntityFieldValue(uint classnum, int entnum, int offset);
void __cdecl Scr_SetStructField(uint structId, uint index);
void __cdecl Scr_SetDynamicEntityField(uint entnum, uint classnum, uint index);
void __cdecl Scr_IncTime();
void __cdecl Scr_RunCurrentThreads();
void VM_SetTime();
void __cdecl VM_Resume(uint timeId);
void __cdecl VM_UnarchiveStack(uint startLocalId, VariableStackBuffer* stackValue);
void VM_UnarchiveStack2(uint startLocalId, function_stack_t *stack, VariableStackBuffer *stackValue);
int __cdecl Scr_AddLocalVars(uint localId);
void __cdecl Scr_ResetTimeout();
BOOL __cdecl Scr_IsStackClear();
void __cdecl Scr_StackClear();
void __cdecl Scr_ProfileUpdate();
void __cdecl Scr_ProfileBuiltinUpdate();
void __cdecl Scr_DoProfile(float minTime);
void __cdecl Scr_DoProfileBuiltin(float minTime);
char __cdecl Scr_PrintProfileBuiltinTimes(float minTime);
int __cdecl Scr_BuiltinCompare(_DWORD* a, _DWORD* b);

void Scr_DecTime();

void Scr_AddExecEntThreadNum(int entnum, uint classnum, int handle, uint paramcount);

extern scrVmPub_t scrVmPub;
extern scrVmDebugPub_t scrVmDebugPub;

extern const dvar_s *logScriptTimes;
