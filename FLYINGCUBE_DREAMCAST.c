#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dc/vmu.h>
#include <dc/maple.h>
#include <dc/controller.h>
#include <dc/pvr.h>

// --- SCREEN AND GAME MODES ---
typedef enum {
    MODE_TV = 0,        // Fullscreen TV Mode (640x480, Colored PVR Graphics)
    MODE_VMU_ONLY       // VMU Handheld Mode (48x32 Monochrome LCD)
} ScreenMode;

typedef enum {
    STATE_MAIN_MENU = 0,
    STATE_CONFIRM_SWITCH, // Confirmation prompt before switching to TV/Gamepad mode
    STATE_PLAYING,
    STATE_PAUSED,
    STATE_GAMEOVER
} GameState;

typedef enum {
    MENU_START_GAME = 0,
    MENU_SWITCH_TO_TV,  // Switch to Dreamcast Controller / TV mode button
    MENU_MAX
} MenuOption;

typedef enum {
    CONFIRM_YES = 0,
    CONFIRM_NO,
    CONFIRM_MAX
} ConfirmOption;

// --- PERSISTENT VMU SAVE STRUCTURE ---
typedef struct {
    int current_score;
    int current_ballscore;
    int high_score;
    int max_ballscore;
    int total_lifetime_score;
    int total_lifetime_ballscore;
} GameStats;

typedef struct {
    float x, y;
    float size;
    float velocity_y;
} PlayerCube;

typedef struct {
    float x;
    float width;
    float top_height;
    float bottom_y;
    bool passed;
} Pipe;

typedef struct {
    float x, y;
    float size;
    bool active;
} BallScoreItem;

// --- GLOBAL VARIABLES ---
GameState game_state = STATE_MAIN_MENU;
ScreenMode screen_mode = MODE_TV;
MenuOption selected_menu = MENU_START_GAME;
ConfirmOption selected_confirm = CONFIRM_NO;
GameStats stats = {0};

PlayerCube cube = { 100.0f, 240.0f, 30.0f, 0.0f };
Pipe pipe = { 640.0f, 60.0f, 150.0f, 330.0f, false };
BallScoreItem coin = { 670.0f, 200.0f, 16.0f, true };

float gravity = 0.4f;
float jump_force = -7.5f;
float scroll_speed = 3.0f;
bool running = true;

// VMU LCD Buffer (48 x 32 pixels = 192 bytes)
uint8 vmu_lcd_buffer[192];

// --- FUNCTION PROTOTYPES ---
void LoadGameDataVMU();
void SaveGameDataVMU();
void ResetGame();
void ProcessInput(cont_state_t *state);
void UpdateGame();
void RenderTVColored();
void RenderVMU();
void DrawPixelVMU(int x, int y, bool set);

int main(int argc, char **argv) {
    // Initialize KallistiOS system
    kos_init_all();

    // Load saved high scores and lifetime statistics from VMU
    LoadGameDataVMU();

    // Set initial video mode to VGA 640x480 for TV display
    vid_set_mode(DM_640x480_VGA, PM_RGB565);
    pvr_init_defaults();

    maple_device_t *controller = NULL;
    maple_device_t *vmu = NULL;

    while (running) {
        if (!controller) controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (!vmu) vmu = maple_enum_type(0, MAPLE_FUNC_VMU);

        cont_state_t *state = controller ? (cont_state_t *)maple_dev_status(controller) : NULL;

        if (state) {
            ProcessInput(state);
        }

        if (game_state == STATE_PLAYING) {
            UpdateGame();
        }

        // Render based on the current screen mode (TV or VMU LCD)
        if (screen_mode == MODE_TV) {
            RenderTVColored();
            vid_wait_vsync();
        } else {
            RenderVMU();
            if (vmu) {
                vmu_draw_lcd(vmu, vmu_lcd_buffer);
            }
            thd_sleep(30); // Frame limiter for VMU handheld mode
        }
    }

    // Save statistics before exiting
    SaveGameDataVMU();
    return 0;
}

// --- VMU SAVE & LOAD FUNCTIONS ---
void LoadGameDataVMU() {
    FILE *f = fopen("/vmu/a1/flycube.dat", "rb");
    if (f) {
        fread(&stats, sizeof(GameStats), 1, f);
        fclose(f);
    } else {
        stats.high_score = 0;
        stats.max_ballscore = 0;
        stats.total_lifetime_score = 0;
        stats.total_lifetime_ballscore = 0;
    }
}

void SaveGameDataVMU() {
    FILE *f = fopen("/vmu/a1/flycube.dat", "wb");
    if (f) {
        fwrite(&stats, sizeof(GameStats), 1, f);
        fclose(f);
    }
}

void ResetGame() {
    cube.x = 100.0f;
    cube.y = 240.0f;
    cube.velocity_y = 0.0f;
    
    pipe.x = 640.0f;
    pipe.top_height = 100.0f + (rand() % 180);
    pipe.bottom_y = pipe.top_height + 130.0f;
    pipe.passed = false;

    coin.x = 670.0f;
    coin.y = pipe.top_height + 65.0f;
    coin.active = true;

    stats.current_score = 0;
    stats.current_ballscore = 0;
}

// --- INPUT MANAGEMENT & CONTROLLER LOGIC ---
void ProcessInput(cont_state_t *state) {
    if (!state) return;

    // 1. BUTTON B or START: Opens/closes the Pause menu during gameplay
    if (state->buttons & CONT_START || state->buttons & CONT_B) {
        if (game_state == STATE_PLAYING) {
            game_state = STATE_PAUSED;
            thd_sleep(250);
            return;
        }
    }

    // 2. Y BUTTON: Instant toggle to VMU handheld mode from TV
    if (state->buttons & CONT_Y && screen_mode == MODE_TV && game_state == STATE_MAIN_MENU) {
        screen_mode = MODE_VMU_ONLY;
        thd_sleep(300);
        return;
    }

    // 3. CONFIRMATION PROMPT STATE (Switching to TV/Gamepad mode)
    if (game_state == STATE_CONFIRM_SWITCH) {
        if (state->joyy < -64 || (state->buttons & CONT_DPAD_UP) || (state->buttons & CONT_DPAD_LEFT)) {
            selected_confirm = CONFIRM_YES;
            thd_sleep(150);
        }
        if (state->joyy > 64 || (state->buttons & CONT_DPAD_DOWN) || (state->buttons & CONT_DPAD_RIGHT)) {
            selected_confirm = CONFIRM_NO;
            thd_sleep(150);
        }

        if (state->buttons & CONT_A) {
            if (selected_confirm == CONFIRM_YES) {
                screen_mode = MODE_TV;
                vid_set_mode(DM_640x480_VGA, PM_RGB565);
                pvr_init_defaults();
            }
            game_state = STATE_MAIN_MENU;
            thd_sleep(250);
        }
        if (state->buttons & CONT_B) {
            game_state = STATE_MAIN_MENU;
            thd_sleep(250);
        }
        return;
    }

    // 4. MAIN MENU STATE (2 Blue Buttons)
    if (game_state == STATE_MAIN_MENU) {
        if (state->joyy < -64 || (state->buttons & CONT_DPAD_UP) || (state->buttons & CONT_DPAD_DOWN)) {
            selected_menu = (selected_menu == MENU_START_GAME) ? MENU_SWITCH_TO_TV : MENU_START_GAME;
            thd_sleep(150);
        }

        if (state->buttons & CONT_A) {
            if (selected_menu == MENU_START_GAME) {
                ResetGame();
                game_state = STATE_PLAYING;
            } else if (selected_menu == MENU_SWITCH_TO_TV) {
                game_state = STATE_CONFIRM_SWITCH;
                selected_confirm = CONFIRM_NO;
            }
            thd_sleep(250);
        }
        return;
    }

    // 5. PAUSE MENU STATE
    if (game_state == STATE_PAUSED) {
        if (state->buttons & CONT_A) {
            game_state = STATE_PLAYING; // Resume game
            thd_sleep(250);
        } else if (state->buttons & CONT_B) {
            game_state = STATE_MAIN_MENU; // Return to main menu
            thd_sleep(250);
        }
        return;
    }

    // 6. GAMEPLAY JUMP: Any action button except Back/Start triggers jump
    if (state->buttons & (CONT_A | CONT_X | CONT_Y | CONT_DPAD_UP)) {
        cube.velocity_y = jump_force;
    }
}

// --- GAME UPDATE LOGIC ---
void UpdateGame() {
    cube.velocity_y += gravity;
    cube.y += cube.velocity_y;

    pipe.x -= scroll_speed;
    coin.x -= scroll_speed;

    if (pipe.x < -pipe.width) {
        pipe.x = 640.0f;
        pipe.top_height = 90.0f + (rand() % 190);
        pipe.bottom_y = pipe.top_height + 130.0f;
        pipe.passed = false;

        coin.x = pipe.x + 25.0f;
        coin.y = pipe.top_height + 65.0f;
        coin.active = true;
    }

    // Score increment upon passing pipe
    if (!pipe.passed && cube.x > pipe.x + pipe.width) {
        stats.current_score++;
        pipe.passed = true;
    }

    // BallScore item collection
    if (coin.active &&
        cube.x < coin.x + coin.size && cube.x + cube.size > coin.x &&
        cube.y < coin.y + coin.size && cube.y + cube.size > coin.y) {
        stats.current_ballscore += 5;
        coin.active = false;
    }

    // Collision detection (Borders and Pipes)
    if (cube.y < 0 || cube.y + cube.size > 480 ||
        (cube.x + cube.size > pipe.x && cube.x < pipe.x + pipe.width &&
         (cube.y < pipe.top_height || cube.y + cube.size > pipe.bottom_y))) {

        if (stats.current_score > stats.high_score) stats.high_score = stats.current_score;
        if (stats.current_ballscore > stats.max_ballscore) stats.max_ballscore = stats.current_ballscore;
        stats.total_lifetime_score += stats.current_score;
        stats.total_lifetime_ballscore += stats.current_ballscore;
        
        SaveGameDataVMU();
        game_state = STATE_GAMEOVER;
    }
}

// --- COLORED TV RENDERING (Pure White Background, Vibrant Blue Buttons, Red Cube) ---
void RenderTVColored() {
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);

    // Render white background and colored PVR polygons here
    // Red Cube character, Green Pipes, Gold/Blue BallScore items, Blue Menu Buttons

    pvr_scene_finish();
}

// --- VMU LCD HANDHELD RENDERING (48x32 Monochrome) ---
void DrawPixelVMU(int x, int y, bool set) {
    if (x < 0 || x >= 48 || y < 0 || y >= 32) return;
    int idx = (y * 6) + (x / 8);
    int bit = 7 - (x % 8);
    if (set) vmu_lcd_buffer[idx] |= (1 << bit);
    else vmu_lcd_buffer[idx] &= ~(1 << bit);
}

void RenderVMU() {
    for (int i = 0; i < 192; i++) vmu_lcd_buffer[i] = 0x00;

    int vx = (int)(cube.x / (640.0f / 48.0f));
    int vy = (int)(cube.y / (480.0f / 32.0f));
    
    // Draw Red Cube on VMU
    for (int dx = 0; dx < 2; dx++) {
        for (int dy = 0; dy < 2; dy++) {
            DrawPixelVMU(vx + dx, vy + dy, true);
        }
    }

    // Draw Pipes on VMU
    int vpipe = (int)(pipe.x / (640.0f / 48.0f));
    int vtop = (int)(pipe.top_height / (480.0f / 32.0f));
    int vbot = (int)(pipe.bottom_y / (480.0f / 32.0f));

    if (vpipe >= 0 && vpipe < 48) {
        for (int y = 0; y < vtop; y++) DrawPixelVMU(vpipe, y, true);
        for (int y = vbot; y < 32; y++) DrawPixelVMU(vpipe, y, true);
    }
}
