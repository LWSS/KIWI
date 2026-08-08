#pragma once
#include <qcommon/graph.h>

enum DevGuiInputState : int
{                                       // ...
    SCROLL_NONE = 0x0,
    SCROLL_PRESSED = 0x1,
    SCROLL_STALLED = 0x2,
    SCROLL_HELD = 0x3,
};
enum DevGuiTokenResult : int
{                                       // ...
    DEVGUI_TOKEN_ERROR = 0x0,
    DEVGUI_TOKEN_MORE = 0x1,
    DEVGUI_TOKEN_LAST = 0x2,
};

struct DevGuiInput // sizeof=0x70
{                                       // ...
    float mousePos[2];                  // ...
    bool buttonDown[11];                // ...
    bool prevButtonDown[11];            // ...
    // padding byte
    // padding byte
    float scrollScale;                  // ...
    DevGuiInputState digitalStates[2];  // ...
    float digitalAxis[2];               // ...
    float digitalTimes[2];              // ...
    DevGuiInputState analogStates[2];   // ...
    float analogAxis[2];                // ...
    float analogTimes[2];
    __int16 menuScroll[2];              // ...
    float menuScrollTime[2];            // ...
    float digitalSliderTime;            // ...
    float analogSliderTime;             // ...
    float sliderScrollTime;             // ...
    float sliderScrollMaxTimeStep;      // ...
};
static_assert(sizeof(DevGuiInput) == 0x70);

union DevMenuChild // sizeof=0x4
{                                       // ...
    const char *command;
    const dvar_s *dvar;
    DevGraph *graph;
    uint16_t menu;
};
static_assert(sizeof(DevMenuChild) == 0x4);

struct DevMenuItem // sizeof=0x28
{                                       // ...
    char label[26];                     // ...
    uint8_t childType;          // ...
    uint8_t childMenuMemory;    // ...
    __int16 sortKey;
    uint16_t nextSibling;       // ...
    uint16_t prevSibling;       // ...
    uint16_t parent;            // ...
    DevMenuChild child;                 // ...
};
static_assert(sizeof(DevMenuItem) == 0x28);

struct devguiGlob_t // sizeof=0x5E10
{                                       // ...
    DevMenuItem menus[600];             // ...
    DevMenuItem *nextFreeMenu;          // ...
    DevMenuItem topmostMenu;            // ...
    bool bindNextKey;                   // ...
    bool isActive;                      // ...
    bool editingMenuItem;               // ...
    // padding byte
    uint16_t selectedMenu;      // ...
    // padding byte
    // padding byte
    int selRow;                         // ...
    bool editingKnot;                   // ...
    // padding byte
    // padding byte
    // padding byte
    int top;                            // ...
    int bottom;                         // ...
    int left;                           // ...
    int right;                          // ...
    int sliderWidth;                    // ...
};
static_assert(sizeof(devguiGlob_t) == 0x5E10);

// devgui
void __cdecl TRACK_devgui();
void __cdecl DevGui_AddDvar(const char *path, const dvar_s *dvar);
devguiGlob_t *__cdecl DevGui_GetMenu(uint16_t handle);
uint16_t __cdecl DevGui_ConstructPath_r(uint16_t parent, const char *path);
uint16_t __cdecl DevGui_RegisterMenu(uint16_t parentHandle, const char *label, __int16 sortKey);
uint16_t __cdecl DevGui_CreateMenu(uint16_t parentHandle, const char *label, __int16 sortKey);
uint16_t __cdecl DevGui_GetMenuHandle(DevMenuItem *menu);
int __cdecl DevGui_CompareMenus(const DevMenuItem *menu0, const DevMenuItem *menu1);
uint16_t __cdecl DevGui_FindMenu(uint16_t parentHandle, const char *label);
DevGuiTokenResult __cdecl DevGui_PathToken(const char **pathInOut, char *label, __int16 *sortKeyOut);
char __cdecl DevGui_IsValidPath(const char *path);
void __cdecl DevGui_AddCommand(const char *path, char *command);
void __cdecl DevGui_AddGraph(const char *path, DevGraph *graph);
void __cdecl DevGui_RemoveMenu(const char *path);
void __cdecl DevGui_FreeMenu_r(uint16_t handle);
void __cdecl DevGui_OpenMenu(const char *path);
bool __cdecl DevGui_EditableMenuItem(const DevMenuItem *menu);
void __cdecl DevGui_Draw(int localClientNum);
uint16_t __cdecl DevGui_GetMenuParent(uint16_t handle);
void __cdecl DevGui_DrawMenu(uint16_t menuHandle, uint16_t activeChild, int *origin);
void __cdecl DevGui_DrawMenuVertically(const DevMenuItem *menu, uint16_t activeChild, int *origin);
bool __cdecl DevGui_MenuItemDisabled(const DevMenuItem *menu);
int __cdecl DevGui_SubMenuTextWidth();
int __cdecl DevGui_MaxChildMenuWidth(const DevMenuItem *menu);
int __cdecl DevGui_MenuItemWidth(const DevMenuItem *menu);
void __cdecl DevGui_DrawMenuHorizontally(const DevMenuItem *menu, uint16_t activeChild, int *origin);
void __cdecl DevGui_ChooseOrigin(int *origin);
void __cdecl DevGui_DrawSliders(const DevMenuItem *menu);
void __cdecl DevGui_DrawSliderPath(int x, int y);
int __cdecl DevGui_GetSliderPath(uint16_t menuHandle, char *path, int pathLen);
void __cdecl DevGui_DrawSingleSlider(
    int x,
    int y,
    int rowWidth,
    int rowHeight,
    float fraction,
    const uint8_t *knobColor);
void __cdecl DevGui_DrawDvarValue(int x, int y, const dvar_s *dvar);
int __cdecl DevGui_DvarRowCount(const dvar_s *dvar);
void DevGui_DrawBindNextKey();
void __cdecl DevGui_DrawGraph(const DevMenuItem *menu, int localClientNum);
void __cdecl DevGui_Init();
const dvar_s *DevGui_RegisterDvars();
void __cdecl DevGui_Shutdown();
void DevGui_MenuShutdown();
void __cdecl DevGui_KeyPressed(int key);
void __cdecl DevGui_Update(int localClientNum, float deltaTime);
void DevGui_MoveSelectionHorizontally();
void DevGui_MoveLeft();
void DevGui_SelectPrevMenuItem();
void DevGui_SelectTopLevelChild();
void __cdecl DevGui_AdvanceChildNum(int numberToAdvance);
void DevGui_MoveRight();
void DevGui_SelectNextMenuItem();
void DevGui_MoveSelectionVertically();
void DevGui_MoveUp();
void DevGui_MoveDown();
void __cdecl DevGui_Accept(int localClientNum);
void DevGui_Reject();
int DevGui_UpdateSelection();
int DevGui_ScrollUp();
const dvar_s *__cdecl DevGui_SelectedDvar();
int DevGui_ScrollUpInternal();
int DevGui_ScrollDown();
int DevGui_ScrollDownInternal();
void __cdecl DevGui_UpdateDvar(float deltaTime);
float __cdecl DevGui_PickFloatScrollStep(float min, float max);
void __cdecl DevGui_UpdateGraph(int localClientNum, float deltaTime);
void __cdecl DevGui_AddGraphKnot(DevGraph *graph, int localClientNum);
void __cdecl DevGui_RemoveGraphKnot(DevGraph *graph, int localClientNum);
void __cdecl DevGui_Toggle();
void DevGui_Reset();
bool __cdecl DevGui_IsActive();

extern devguiGlob_t devguiGlob;

// devgui_input
enum DevGuiInputAxis : int
{                                       // ...
    SCROLL_XAXIS = 0x0,
    SCROLL_YAXIS = 0x1,
    SCROLL_AXIS_COUNT = 0x2,
};

enum DevGuiInputButton : int
{                                       // ...
    INPUT_UP = 0x0,
    INPUT_DOWN = 0x1,
    INPUT_LEFT = 0x2,
    INPUT_RIGHT = 0x3,
    INPUT_ACCEPT = 0x4,
    INPUT_REJECT = 0x5,
    INPUT_BIND = 0x6,
    INPUT_GRAPH_EDIT = 0x7,
    INPUT_GRAPH_ADD = 0x8,
    INPUT_GRAPH_REMOVE = 0x9,
    INPUT_GRAPH_SAVE = 0xA,
    INPUT_COUNT = 0xB,
};

void __cdecl DevGui_InputInit();
void __cdecl DevGui_InputShutdown();
char __cdecl DevGui_InputUpdate(int localClientNum, float deltaTime);
void __cdecl DevGui_UpdateScrollInputs(int localClientNum);
void __cdecl DevGui_UpdateScrollStates(float deltaTime, DevGuiInputState *states, float *axis, float *times);
void __cdecl DevGui_UpdateMenuScroll(float deltaTime);
void __cdecl DevGui_MouseEvent(int dx, int dy);
__int16 __cdecl DevGui_GetMenuScroll(DevGuiInputAxis axis);
int __cdecl DevGui_UpdateIntScroll(float deltaTime, int value, int min, int max, DevGuiInputAxis axis);
double __cdecl DevGui_UpdateFloatScroll(
    float deltaTime,
    float value,
    float min,
    float max,
    float step,
    DevGuiInputAxis axis);
bool __cdecl DevGui_IsButtonDown(DevGuiInputButton button);
bool __cdecl DevGui_IsButtonPressed(DevGuiInputButton button);
bool __cdecl DevGui_IsButtonReleased(DevGuiInputButton button);



// devgui_util
uint __cdecl DevGui_GetScreenWidth();
uint __cdecl DevGui_GetScreenHeight();
void __cdecl DevGui_DrawBox(int x, int y, int w, int h, const uint8_t *color);
void __cdecl DevGui_DrawBoxCentered(int centerX, int centerY, int w, int h, const uint8_t *color);
void __cdecl DevGui_DrawBevelBox(int x, int y, int w, int h, float shade, const uint8_t *color);
void __cdecl DevGui_DrawQuad(const int (*vtxs)[2], const float *color);
void __cdecl DevGui_DrawLine(float *start, float *end, int width, const uint8_t *color);
void __cdecl DevGui_DrawFont(int x, int y, const uint8_t *color, char *text);
int __cdecl DevGui_GetFontWidth(const char *text);
int __cdecl DevGui_GetFontHeight();
