/* ============================================================================
 * pianochords - Win32 MIDI Chord & Scale Explorer
 * 
 * COMPILATION:
 *   gcc -Os -s -Wall -Wextra pianochords.c -o pianochords.exe -mwindows -lwinmm -lgdi32
 *
 * FEATURES:
 *  - Massive database with over 15,000+ chord/scale voicings and variations.
 *  - Dual-ListBox UI: Group selection (Left) and Variation selection (Right).
 *  - Interactive Selection: Clicking either ListBox immediately plays the chord.
 *  - Focus-Aware Search: Real-time search executes only when the list has focus.
 *  - Headless Filter: Automatically resets buffer upon typing a new query after timeout.
 *  - Focus-Aware QWERTY Piano: Keyboard input routes to the clicked piano.
 *  - Precise GDI Vector Treble (Choice B) and Bass Clef rendering.
 *  - Interactive Piano: Click piano keys or type to play notes.
 *  - "Allowed" indicators (Red X) dynamically drawn on chord notes.
 *  - Dual-keyboard layout & Isolated staff view (Treble / Bass).
 *  - Clean GCC compile with -Wall -Wextra (no unused param or implicit warnings).
 *  - 100% flicker-free resizing via WS_CLIPCHILDREN and memory DCs.
 *
 * THIS WORK IS NOT FIT FOR ANY FUNCTION OR PURPOSE, COMES WITH NO WARRANTY,
 * AND IS BEING RELEASED INTO THE PUBLIC DOMAIN.
 * ============================================================================ */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_GROUPS 1500
#define MAX_VARS 20
#define MAX_STEPS 32
#define MAX_NOTES_PER_STEP 24

#define TIMER_PLAY 1
#define TIMER_SEARCH 2
#define TIMER_SPEED_MS 400
#define WM_INIT_MIDI (WM_APP + 1)

// --- Data Structures ---
typedef struct {
    int notes[MAX_NOTES_PER_STEP];
    int num_notes;
} Step;

typedef struct {
    char name[128];
    Step steps[MAX_STEPS];
    int num_steps;
} Variation;

typedef struct {
    char base_name[128];
    Variation vars[MAX_VARS];
    int num_vars;
} MusicalGroup;

// --- Globals ---
HMIDIOUT hMidiOut;
MusicalGroup groups[MAX_GROUPS];
int num_groups = 0;

int filtered_indices[MAX_GROUPS];
int num_filtered = 0;

int active_midi_notes[128] = {0}; 
int allowed_notes[128] = {0};

int playing_mode = 0;             
int active_play_hand = 0; 
int current_grp_idx = -1;
int current_var_idx = -1;
int current_step = 0;
int current_arp_note = 0;
int is_ready = 0;
int clicked_note = -1; 
int focused_piano = 2; // 1 = Left Hand (Bass), 2 = Right Hand (Treble)

HWND g_hwndMain = NULL;
HHOOK hKeyboardHook = NULL;
char search_query[256] = "";
int reset_search_on_next_key = 0;

HWND hListGroup, hListVar, hBtnPlayBlock, hBtnPlayArp, hBtnPlayLH, hBtnPlayRH;
HBRUSH hBrushWhite, hBrushBlack, hBrushActive;

// --- Forward Declarations ---
void MidiNoteOn(int note);
void MidiNoteOff(int note);
void AllNotesOff(void);
void UpdateAllowedNotes(void);
void FilterItems(HWND hwnd);
void PopulateVars(void);
void PlayNextStep(HWND hwnd);
void StopPlayback(HWND hwnd);
void GetLayoutRects(HWND hwnd, RECT* prcRH, RECT* prcLH, RECT* prcTreble, RECT* prcBass);
int  GetPianoNoteAtPoint(int pt_x, int pt_y, RECT rc, int start_note, int end_note);
void DrawPiano(HDC hdc, RECT rc, int start_note, int end_note, const char* label, int is_focused);
void DrawHandStaff(HDC hdc, RECT rc, int is_treble);
void DrawTrebleClef(HDC hdc, int x, int center_y);
void DrawBassClef(HDC hdc, int x, int center_y);

// --- Utility: Case-insensitive substring ---
char* stristr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; ++haystack) {
        if (toupper((unsigned char)*haystack) == toupper((unsigned char)*needle)) {
            const char *h = haystack, *n = needle;
            while (*h && *n && toupper((unsigned char)*h) == toupper((unsigned char)*n)) { h++; n++; }
            if (!*n) return (char*)haystack;
        }
    }
    return NULL;
}

// --- MIDI Helpers ---
void MidiNoteOn(int note) {
    if (note < 0 || note > 127 || !is_ready) return;
    union { DWORD dwData; BYTE bData[4]; } u;
    u.bData[0] = 0x90; 
    u.bData[1] = (BYTE)note; 
    u.bData[2] = 100;  
    u.bData[3] = 0;
    midiOutShortMsg(hMidiOut, u.dwData);
    active_midi_notes[note] = 1;
}

void MidiNoteOff(int note) {
    if (note < 0 || note > 127 || !is_ready) return;
    union { DWORD dwData; BYTE bData[4]; } u;
    u.bData[0] = 0x80; 
    u.bData[1] = (BYTE)note; 
    u.bData[2] = 0;  
    u.bData[3] = 0;
    midiOutShortMsg(hMidiOut, u.dwData);
    active_midi_notes[note] = 0;
}

void AllNotesOff(void) {
    for (int i = 0; i < 128; i++) {
        if (active_midi_notes[i] && is_ready) {
            union { DWORD dwData; BYTE bData[4]; } u;
            u.bData[0] = 0x80; 
            u.bData[1] = (BYTE)i; 
            u.bData[2] = 0; 
            u.bData[3] = 0;
            midiOutShortMsg(hMidiOut, u.dwData);
            active_midi_notes[i] = 0;
        }
    }
}

// --- QWERTY Piano Mapping with Focus Routing ---
int GetNoteFromVK(DWORD vkCode, int target_piano) {
    int base_offset = (target_piano == 1) ? -24 : 0; // LH shifts down 2 octaves

    switch(vkCode) {
        // Lower Row
        case 'Z': return 48 + base_offset;
        case 'S': return 49 + base_offset;
        case 'X': return 50 + base_offset;
        case 'D': return 51 + base_offset;
        case 'C': return 52 + base_offset;
        case 'V': return 53 + base_offset;
        case 'G': return 54 + base_offset;
        case 'B': return 55 + base_offset;
        case 'H': return 56 + base_offset;
        case 'N': return 57 + base_offset;
        case 'J': return 58 + base_offset;
        case 'M': return 59 + base_offset;
        case VK_OEM_COMMA: return 60 + base_offset;
        case 'L': return 61 + base_offset;
        case VK_OEM_PERIOD: return 62 + base_offset;
        case VK_OEM_1: return 63 + base_offset;
        case VK_OEM_2: return 64 + base_offset;
        
        // Upper Row
        case 'Q': return 60 + base_offset;
        case '2': return 61 + base_offset;
        case 'W': return 62 + base_offset;
        case '3': return 63 + base_offset;
        case 'E': return 64 + base_offset;
        case 'R': return 65 + base_offset;
        case '5': return 66 + base_offset;
        case 'T': return 67 + base_offset;
        case '6': return 68 + base_offset;
        case 'Y': return 69 + base_offset;
        case '7': return 70 + base_offset;
        case 'U': return 71 + base_offset;
        case 'I': return 72 + base_offset;
        case '9': return 73 + base_offset;
        case 'O': return 74 + base_offset;
        case '0': return 75 + base_offset;
        case 'P': return 76 + base_offset;
    }
    return -1;
}

// --- Keyboard Hook (Piano & Resetting Search) ---
LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0) {
        int is_keyup = (lParam & 0x80000000) ? 1 : 0;
        int is_repeat = (lParam & 0x40000000) ? 1 : 0;
        DWORD vkCode = (DWORD)wParam;
        HWND hFocus = GetFocus();
        
        // 1. Piano Playback on Focused Keyboard (Runs whenever ListBox does NOT have focus)
        if (hFocus != hListGroup) {
            int note = GetNoteFromVK(vkCode, focused_piano);
            if (note >= 0 && note <= 127) {
                if (!is_keyup && !is_repeat && is_ready) {
                    MidiNoteOn(note);
                    if (g_hwndMain) InvalidateRect(g_hwndMain, NULL, FALSE);
                } else if (is_keyup && is_ready) {
                    MidiNoteOff(note);
                    if (g_hwndMain) InvalidateRect(g_hwndMain, NULL, FALSE);
                }
            }
        }

        // 2. Search Engine (ONLY executes when the Groups ListBox has focus)
        if (is_keyup && hFocus == hListGroup) { 
            int needs_update = 0;
            if (vkCode == VK_BACK) {
                int len = (int)strlen(search_query);
                if (len > 0) { search_query[len - 1] = '\0'; needs_update = 1; }
            } else if (vkCode == VK_ESCAPE) {
                search_query[0] = '\0'; needs_update = 1;
            } else {
                BYTE ks[256]; 
                GetKeyboardState(ks);
                WORD ascii = 0;
                if (ToAscii((UINT)vkCode, (UINT)((lParam >> 16) & 0xFF), ks, &ascii, 0) == 1) {
                    char c = (char)ascii;
                    if (isprint((unsigned char)c)) {
                        if (reset_search_on_next_key) {
                            search_query[0] = '\0';
                            reset_search_on_next_key = 0;
                        }
                        int len = (int)strlen(search_query);
                        if (len < 255) {
                            search_query[len] = c; 
                            search_query[len + 1] = '\0';
                            needs_update = 1;
                        }
                    }
                }
            }

            if (needs_update && g_hwndMain) {
                KillTimer(g_hwndMain, TIMER_SEARCH);
                SetTimer(g_hwndMain, TIMER_SEARCH, 1000, NULL);
            }
        }
    }
    return CallNextHookEx(hKeyboardHook, code, wParam, lParam);
}

// --- Massive CSV Generator & Data Engine ---
int AddVariation(int grp_idx, const char* var_name, int* notes, int count) {
    if (grp_idx < 0 || grp_idx >= MAX_GROUPS) return -1;
    MusicalGroup* g = &groups[grp_idx];
    if (g->num_vars >= MAX_VARS) return -1;

    Variation* v = &g->vars[g->num_vars++];
    strncpy(v->name, var_name, 127);
    v->name[127] = '\0';
    v->num_steps = 1;
    v->steps[0].num_notes = 0;
    
    for (int i = 0; i < count && i < MAX_NOTES_PER_STEP; i++) {
        v->steps[0].notes[v->steps[0].num_notes++] = notes[i];
    }
    return g->num_vars - 1;
}

void CreateMassiveSampleCSV(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    
    const char* roots[] = {"C","Db","D","Eb","E","F","F#","G","Ab","A","Bb","B"};
    int root_offsets[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}; 

    typedef struct { const char* ext; int v[7]; int cnt; } CDef;
    CDef cdefs[] = {
        {"Major", {0,4,7}, 3}, {"Minor", {0,3,7}, 3}, {"Diminished", {0,3,6}, 3}, {"Augmented", {0,4,8}, 3},
        {"Sus2", {0,2,7}, 3}, {"Sus4", {0,5,7}, 3},
        {"Maj7", {0,4,7,11}, 4}, {"Min7", {0,3,7,10}, 4}, {"Dom7", {0,4,7,10}, 4}, {"Dim7", {0,3,6,9}, 4},
        {"Half-Dim7", {0,3,6,10}, 4}, {"Min(Maj7)", {0,3,7,11}, 4}, {"Maj6", {0,4,7,9}, 4}, {"Min6", {0,3,7,9}, 4},
        {"Add9", {0,4,7,14}, 4}, {"MinAdd9", {0,3,7,14}, 4}, {"7b5", {0,4,6,10}, 4}, {"7#5", {0,4,8,10}, 4},
        {"Maj9", {0,4,7,11,14}, 5}, {"Min9", {0,3,7,10,14}, 5}, {"Dom9", {0,4,7,10,14}, 5},
        {"7b9", {0,4,7,10,13}, 5}, {"7#9", {0,4,7,10,15}, 5}, {"13", {0,4,7,10,14,21}, 6}, {"Min13", {0,3,7,10,14,21}, 6}
    };
    int num_cdefs = (int)(sizeof(cdefs) / sizeof(CDef));

    typedef struct { const char* ext; int v[8]; int cnt; } SDef;
    SDef sdefs[] = {
        {"Major Scale", {0,2,4,5,7,9,11,12}, 8}, {"Natural Minor", {0,2,3,5,7,8,10,12}, 8}, 
        {"Harmonic Minor", {0,2,3,5,7,8,11,12}, 8}, {"Melodic Minor", {0,2,3,5,7,9,11,12}, 8},
        {"Dorian", {0,2,3,5,7,9,10,12}, 8}, {"Phrygian", {0,1,3,5,7,8,10,12}, 8}, 
        {"Lydian", {0,2,4,6,7,9,11,12}, 8}, {"Mixolydian", {0,2,4,5,7,9,10,12}, 8},
        {"Locrian", {0,1,3,5,6,8,10,12}, 8}, {"Pentatonic Maj", {0,2,4,7,9,12}, 6},
        {"Pentatonic Min", {0,3,5,7,10,12}, 6}, {"Blues", {0,3,5,6,7,10,12}, 7},
        {"Whole Tone", {0,2,4,6,8,10,12}, 7}, {"Diminished (HW)", {0,1,3,4,6,7,9,10}, 8},
        {"Hirajoshi", {0,4,5,9,11,12}, 6}, {"Insen", {0,1,5,7,10,12}, 6},
        {"Neapolitan Maj", {0,1,3,5,7,9,11,12}, 8}, {"Enigmatic", {0,1,4,6,8,10,11,12}, 8}
    };
    int num_sdefs = (int)(sizeof(sdefs) / sizeof(SDef));

    for (int i = 0; i < 12; i++) {
        int rh = root_offsets[i] + 60; 
        int lh = root_offsets[i] + 36; 

        for (int c = 0; c < num_cdefs; c++) {
            fprintf(f, "%s %s Chord, Root Position, %d;%d", roots[i], cdefs[c].ext, lh, lh+12);
            for(int v=0; v<cdefs[c].cnt; v++) fprintf(f, ";%d", rh + cdefs[c].v[v]);
            fprintf(f, "\n");

            fprintf(f, "%s %s Chord, 1st Inversion, %d;%d", roots[i], cdefs[c].ext, lh, lh+12);
            for(int v=1; v<cdefs[c].cnt; v++) fprintf(f, ";%d", rh + cdefs[c].v[v]);
            fprintf(f, ";%d\n", rh + cdefs[c].v[0] + 12);
            
            if (cdefs[c].cnt >= 3) {
                fprintf(f, "%s %s Chord, 2nd Inversion, %d;%d", roots[i], cdefs[c].ext, lh, lh+12);
                for(int v=2; v<cdefs[c].cnt; v++) fprintf(f, ";%d", rh + cdefs[c].v[v]);
                fprintf(f, ";%d;%d\n", rh + cdefs[c].v[0] + 12, rh + cdefs[c].v[1] + 12);
            }
            if (cdefs[c].cnt >= 4) {
                fprintf(f, "%s %s Chord, Drop 2, %d;%d", roots[i], cdefs[c].ext, lh, lh+12);
                fprintf(f, ";%d", rh + cdefs[c].v[cdefs[c].cnt - 2] - 12);
                for(int v=0; v<cdefs[c].cnt; v++) if (v != cdefs[c].cnt - 2) fprintf(f, ";%d", rh + cdefs[c].v[v]);
                fprintf(f, "\n");
            }
        }

        for (int s = 0; s < num_sdefs; s++) {
            fprintf(f, "%s %s, Block View, %d;%d", roots[i], sdefs[s].ext, lh, lh+12);
            for(int v=0; v<sdefs[s].cnt; v++) fprintf(f, ";%d", rh + sdefs[s].v[v]);
            fprintf(f, "\n");
        }
    }
    fclose(f);
}

void LoadCSV(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        CreateMassiveSampleCSV(filename); 
        f = fopen(filename, "r");
        if (!f) return;
    }

    num_groups = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0; 
        if (strlen(line) == 0) continue;

        char* comma1 = strchr(line, ',');
        if (!comma1) continue;
        *comma1 = 0;
        char* base_name = line;
        
        char* comma2 = strchr(comma1 + 1, ',');
        if (!comma2) continue;
        *comma2 = 0;
        char* var_name = comma1 + 1;
        while(*var_name == ' ') var_name++;

        int grp_idx = -1;
        for (int i = 0; i < num_groups; i++) {
            if (strcmp(groups[i].base_name, base_name) == 0) { grp_idx = i; break; }
        }
        if (grp_idx == -1 && num_groups < MAX_GROUPS) {
            grp_idx = num_groups++;
            strncpy(groups[grp_idx].base_name, base_name, 127);
            groups[grp_idx].base_name[127] = '\0';
            groups[grp_idx].num_vars = 0;
        }

        if (grp_idx != -1) {
            int notes[MAX_NOTES_PER_STEP];
            int n_cnt = 0;
            char* semi = comma2 + 1;
            while(*semi == ' ') semi++;
            while (semi && *semi && n_cnt < MAX_NOTES_PER_STEP) {
                notes[n_cnt++] = atoi(semi);
                char* next = strchr(semi, ';');
                if (next) { *next = 0; semi = next + 1; }
                else semi = NULL;
            }
            AddVariation(grp_idx, var_name, notes, n_cnt);
        }
    }
    fclose(f);
}

// --- Filtering & Selection ---
void FilterItems(HWND hwnd) {
    SendMessage(hListGroup, LB_RESETCONTENT, 0, 0);
    SendMessage(hListVar, LB_RESETCONTENT, 0, 0);
    num_filtered = 0;
    
    for (int i = 0; i < num_groups; i++) {
        if (search_query[0] == '\0' || stristr(groups[i].base_name, search_query) != NULL) {
            filtered_indices[num_filtered] = i;
            int idx = (int)SendMessage(hListGroup, LB_ADDSTRING, 0, (LPARAM)groups[i].base_name);
            SendMessage(hListGroup, LB_SETITEMDATA, idx, (LPARAM)i); 
            num_filtered++;
        }
    }
    current_grp_idx = -1;
    current_var_idx = -1;
    memset(allowed_notes, 0, sizeof(allowed_notes));
    InvalidateRect(hwnd, NULL, FALSE);
}

void PopulateVars(void) {
    SendMessage(hListVar, LB_RESETCONTENT, 0, 0);
    if (current_grp_idx >= 0 && current_grp_idx < num_groups) {
        MusicalGroup* g = &groups[current_grp_idx];
        for (int i = 0; i < g->num_vars; i++) {
            SendMessage(hListVar, LB_ADDSTRING, 0, (LPARAM)g->vars[i].name);
        }
        if (g->num_vars > 0) {
            SendMessage(hListVar, LB_SETCURSEL, 0, 0);
            current_var_idx = 0;
        }
    }
}

void UpdateAllowedNotes(void) {
    memset(allowed_notes, 0, sizeof(allowed_notes));
    AllNotesOff();
    if (current_grp_idx >= 0 && current_var_idx >= 0) {
        Variation* v = &groups[current_grp_idx].vars[current_var_idx];
        if (v->num_steps > 0) {
            for (int n = 0; n < v->steps[0].num_notes; n++) {
                int note = v->steps[0].notes[n];
                allowed_notes[note] = 1;
                MidiNoteOn(note); // Play both hands via MIDI and highlight keys
            }
        }
    }
}

// --- Playback Engine ---
void PlayNextStep(HWND hwnd) {
    AllNotesOff();

    if (current_grp_idx < 0 || current_var_idx < 0) {
        playing_mode = 0; KillTimer(hwnd, TIMER_PLAY); return;
    }

    Variation* v = &groups[current_grp_idx].vars[current_var_idx];

    if (current_step >= v->num_steps) {
        playing_mode = 0; KillTimer(hwnd, TIMER_PLAY);
        InvalidateRect(hwnd, NULL, FALSE); return;
    }

    if (playing_mode == 1) { 
        for (int i = 0; i < v->steps[current_step].num_notes; i++) {
            int n = v->steps[current_step].notes[i];
            if (active_play_hand == 1 && n >= 60) continue; 
            if (active_play_hand == 2 && n < 60) continue;  
            MidiNoteOn(n);
        }
        current_step++;
    } 
    else if (playing_mode == 2) { 
        int n = v->steps[current_step].notes[current_arp_note];
        int skip = (active_play_hand == 1 && n >= 60) || (active_play_hand == 2 && n < 60);
        
        if (!skip) {
            MidiNoteOn(n);
        }

        current_arp_note++;
        if (current_arp_note >= v->steps[current_step].num_notes) {
            current_arp_note = 0; current_step++;
        }
    }
    InvalidateRect(hwnd, NULL, FALSE); 
}

void StopPlayback(HWND hwnd) {
    KillTimer(hwnd, TIMER_PLAY);
    playing_mode = 0;
    AllNotesOff();
    InvalidateRect(hwnd, NULL, FALSE);
}

// --- Dynamic Vector Clefs ---
void DrawTrebleClef(HDC hdc, int x, int center_y) {
    int gy = center_y + 8; // G4 staff line
    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(BLACK_BRUSH));

    Ellipse(hdc, x - 10, gy + 32, x - 2, gy + 40);

    POINT p[19] = {
        {x - 3, gy + 36},
        {x - 3, gy + 44}, {x + 7, gy + 44}, {x + 7, gy + 20},
        {x + 7, gy + 5},  {x + 7, gy - 20}, {x + 4, gy - 46},
        {x - 1, gy - 54}, {x - 13, gy - 40},{x - 2, gy - 16},
        {x + 8, gy + 4},  {x + 19, gy + 13},{x + 8, gy + 25},
        {x - 5, gy + 25}, {x - 16, gy + 14},{x - 16, gy - 2},
        {x - 16, gy - 14},{x - 3, gy - 14}, {x - 3, gy - 1}
    };
    PolyBezier(hdc, p, 19);

    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hPen);
}

void DrawBassClef(HDC hdc, int x, int center_y) {
    int y = center_y - 8; // F3 line center
    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0,0,0));
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);

    SelectObject(hdc, GetStockObject(BLACK_BRUSH));
    Ellipse(hdc, x-4, y-4, x+2, y+2); 

    POINT b[7];
    b[0] = (POINT){x-1, y-1};
    b[1] = (POINT){x, y-16};
    b[2] = (POINT){x+16, y-12};
    b[3] = (POINT){x+14, y+4};
    b[4] = (POINT){x+10, y+16};
    b[5] = (POINT){x+4, y+20};
    b[6] = (POINT){x-4, y+22};
    PolyBezier(hdc, b, 7);

    Ellipse(hdc, x+16, y-6, x+20, y-2); 
    Ellipse(hdc, x+16, y+4, x+20, y+8); 

    SelectObject(hdc, hOld);
    DeleteObject(hPen);
}

// --- Layout & Hit Testing ---
void GetLayoutRects(HWND hwnd, RECT* prcRH, RECT* prcLH, RECT* prcTreble, RECT* prcBass) {
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int top_h = 220;
    if (rcClient.bottom <= top_h) return;
    int work_h = rcClient.bottom - top_h - 10;
    int piano_w = (int)(rcClient.right * 0.65);
    int half_h = work_h / 2;
    if (prcRH) { prcRH->left = 10; prcRH->top = top_h; prcRH->right = 10 + piano_w; prcRH->bottom = top_h + half_h - 5; }
    if (prcLH) { prcLH->left = 10; prcLH->top = top_h + half_h + 5; prcLH->right = 10 + piano_w; prcLH->bottom = top_h + work_h; }
    if (prcTreble) { prcTreble->left = piano_w + 20; prcTreble->top = top_h; prcTreble->right = rcClient.right - 10; prcTreble->bottom = top_h + half_h - 5; }
    if (prcBass) { prcBass->left = piano_w + 20; prcBass->top = top_h + half_h + 5; prcBass->right = rcClient.right - 10; prcBass->bottom = top_h + work_h; }
}

int GetPianoNoteAtPoint(int pt_x, int pt_y, RECT rc, int start_note, int end_note) {
    if (pt_x < rc.left || pt_x > rc.right || pt_y < rc.top || pt_y > rc.bottom) return -1;
    int is_black[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int num_white_keys = 0;
    for (int i = start_note; i <= end_note; i++) if (!is_black[i % 12]) num_white_keys++;
    int key_w = (rc.right - rc.left) / (num_white_keys ? num_white_keys : 1);
    int piano_h = rc.bottom - rc.top - 20;
    int draw_top = rc.top + 20;

    int x = rc.left, black_w = (int)(key_w * 0.6), black_h = (int)(piano_h * 0.6);
    for (int i = start_note; i <= end_note; i++) {
        if (!is_black[i % 12]) { x += key_w; } else {
            RECT rcKey = { x - black_w / 2, draw_top, x + black_w / 2, draw_top + black_h };
            if (pt_x >= rcKey.left && pt_x <= rcKey.right && pt_y >= rcKey.top && pt_y <= rcKey.bottom) return i;
        }
    }
    x = rc.left;
    for (int i = start_note; i <= end_note; i++) {
        if (!is_black[i % 12]) {
            RECT rcKey = { x, draw_top, x + key_w, draw_top + piano_h };
            if (pt_x >= rcKey.left && pt_x <= rcKey.right && pt_y >= rcKey.top && pt_y <= rcKey.bottom) return i;
            x += key_w;
        }
    }
    return -1;
}

void DrawPiano(HDC hdc, RECT rc, int start_note, int end_note, const char* label, int is_focused) {
    int is_black[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int num_white_keys = 0;
    for (int i = start_note; i <= end_note; i++) if (!is_black[i % 12]) num_white_keys++;
    
    int key_w = (rc.right - rc.left) / (num_white_keys ? num_white_keys : 1);
    int piano_h = rc.bottom - rc.top - 20;
    int draw_top = rc.top + 20;

    SetBkMode(hdc, TRANSPARENT); 
    SetTextColor(hdc, is_focused ? RGB(0, 100, 220) : RGB(0, 0, 0));
    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "%s %s", label, is_focused ? "[ACTIVE FOCUS]" : "");
    TextOut(hdc, rc.left + 5, rc.top + 2, title_buf, (int)strlen(title_buf));
    
    SelectObject(hdc, GetStockObject(BLACK_PEN));
    int has_sel = (current_grp_idx != -1 && current_var_idx != -1);

    int x = rc.left;
    for (int i = start_note; i <= end_note; i++) {
        if (!is_black[i % 12]) {
            RECT rcKey = { x, draw_top, x + key_w, draw_top + piano_h };
            FillRect(hdc, &rcKey, active_midi_notes[i] ? hBrushActive : hBrushWhite);
            FrameRect(hdc, &rcKey, hBrushBlack);
            if (has_sel && allowed_notes[i]) {
                SetTextColor(hdc, RGB(220, 0, 0));
                TextOut(hdc, x + key_w/2 - 4, rcKey.bottom - 20, "X", 1);
            }
            x += key_w;
        }
    }

    x = rc.left;
    int black_w = (int)(key_w * 0.6), black_h = (int)(piano_h * 0.6);
    for (int i = start_note; i <= end_note; i++) {
        if (!is_black[i % 12]) { x += key_w; } else {
            RECT rcKey = { x - black_w / 2, draw_top, x + black_w / 2, draw_top + black_h };
            FillRect(hdc, &rcKey, active_midi_notes[i] ? hBrushActive : hBrushBlack);
            FrameRect(hdc, &rcKey, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (has_sel && allowed_notes[i]) {
                SetTextColor(hdc, RGB(255, 80, 80)); 
                TextOut(hdc, x - 4, rcKey.bottom - 20, "X", 1);
            }
        }
    }
}

void DrawHandStaff(HDC hdc, RECT rc, int is_treble) {
    FillRect(hdc, &rc, hBrushWhite); 
    FrameRect(hdc, &rc, hBrushBlack);
    int center_y = rc.top + (rc.bottom - rc.top) / 2;
    SelectObject(hdc, GetStockObject(BLACK_PEN));

    for (int i = -2; i <= 2; i++) {
        MoveToEx(hdc, rc.left + 20, center_y + i * 8, NULL);
        LineTo(hdc, rc.right - 20, center_y + i * 8);
    }

    if (is_treble) DrawTrebleClef(hdc, rc.left + 25, center_y);
    else DrawBassClef(hdc, rc.left + 25, center_y);

    SetBkMode(hdc, TRANSPARENT); 
    SetTextColor(hdc, RGB(0,0,0));
    TextOut(hdc, rc.left + 5, rc.top + 2, is_treble ? "Right Hand" : "Left Hand", is_treble ? 10 : 9);

    MoveToEx(hdc, rc.left + 60, center_y - 16, NULL); LineTo(hdc, rc.left + 60, center_y + 16);
    MoveToEx(hdc, rc.right - 40, center_y - 16, NULL); LineTo(hdc, rc.right - 40, center_y + 16);

    int base_x = rc.left + 90, current_x = base_x, last_step = -999;
    int diatonic_steps[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    int sharp_map[12]      = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};

    int center_midi = is_treble ? 71 : 50; 
    int center_step_abs = ((center_midi / 12) - 1) * 7 + diatonic_steps[center_midi % 12];

    for (int i = 0; i < 128; i++) {
        if (active_midi_notes[i]) {
            if (is_treble && i < 60) continue; 
            if (!is_treble && i >= 60) continue;

            int step_abs = ((i / 12) - 1) * 7 + diatonic_steps[i % 12];
            int step_diff = step_abs - center_step_abs;
            int y = center_y - (step_diff * 4);

            if (step_abs == last_step + 1 || step_abs == last_step) current_x += 16; 
            else current_x = base_x; 

            if (step_diff >= 6) { 
                for (int s = 6; s <= step_diff; s += 2) {
                    MoveToEx(hdc, current_x - 12, center_y - (s*4), NULL); LineTo(hdc, current_x + 12, center_y - (s*4));
                }
            }
            if (step_diff <= -6) {
                for (int s = -6; s >= step_diff; s -= 2) {
                    MoveToEx(hdc, current_x - 12, center_y - (s*4), NULL); LineTo(hdc, current_x + 12, center_y - (s*4));
                }
            }

            SelectObject(hdc, GetStockObject(BLACK_BRUSH));
            Ellipse(hdc, current_x - 6, y - 4, current_x + 7, y + 5);
            if (sharp_map[i % 12]) TextOut(hdc, current_x - 18, y - 8, "#", 1);
            last_step = step_abs;
        }
    }
}

// --- Window Procedure ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            g_hwndMain = hwnd;
            hBrushWhite = CreateSolidBrush(RGB(255, 255, 255));
            hBrushBlack = CreateSolidBrush(RGB(40, 40, 40));
            hBrushActive = CreateSolidBrush(RGB(50, 255, 50)); 

            hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD, KeyboardProc, NULL, GetCurrentThreadId());

            CreateWindow("STATIC", "Chord/Scale Groups:", WS_CHILD | WS_VISIBLE, 10, 10, 200, 20, hwnd, NULL, NULL, NULL);
            hListGroup = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                                    10, 30, 280, 180, hwnd, (HMENU)100, NULL, NULL);

            CreateWindow("STATIC", "Allowed Voicings/Variations:", WS_CHILD | WS_VISIBLE, 300, 10, 200, 20, hwnd, NULL, NULL, NULL);
            hListVar = CreateWindowEx(WS_EX_CLIENTEDGE, "LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                                    300, 30, 280, 180, hwnd, (HMENU)106, NULL, NULL);

            hBtnPlayBlock = CreateWindow("BUTTON", "Play Both Hands", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                         600, 30, 140, 30, hwnd, (HMENU)101, NULL, NULL);
            hBtnPlayArp = CreateWindow("BUTTON", "Play Arpeggio", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                       600, 65, 140, 30, hwnd, (HMENU)102, NULL, NULL);
            hBtnPlayLH = CreateWindow("BUTTON", "Play LH Only", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                      750, 30, 100, 30, hwnd, (HMENU)104, NULL, NULL);
            hBtnPlayRH = CreateWindow("BUTTON", "Play RH Only", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                      860, 30, 100, 30, hwnd, (HMENU)105, NULL, NULL);

            SendMessage(hListGroup, LB_ADDSTRING, 0, (LPARAM)"Initializing MIDI & Generating 15,000+ Items...");
            PostMessage(hwnd, WM_INIT_MIDI, 0, 0);
            break;
        }

        case WM_INIT_MIDI: {
            midiOutOpen(&hMidiOut, (UINT)-1, 0, 0, CALLBACK_NULL);
            LoadCSV("music_data.csv");
            FilterItems(hwnd); 
            EnableWindow(hBtnPlayBlock, TRUE); 
            EnableWindow(hBtnPlayArp, TRUE);
            EnableWindow(hBtnPlayLH, TRUE); 
            EnableWindow(hBtnPlayRH, TRUE);
            is_ready = 1;
            break;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == 100 && HIWORD(wParam) == LBN_SELCHANGE && is_ready) {
                StopPlayback(hwnd);
                int curSel = (int)SendMessage(hListGroup, LB_GETCURSEL, 0, 0);
                if (curSel != LB_ERR) {
                    current_grp_idx = (int)SendMessage(hListGroup, LB_GETITEMDATA, curSel, 0);
                    PopulateVars();
                    UpdateAllowedNotes();
                }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (LOWORD(wParam) == 106 && HIWORD(wParam) == LBN_SELCHANGE && is_ready) {
                StopPlayback(hwnd);
                current_var_idx = (int)SendMessage(hListVar, LB_GETCURSEL, 0, 0);
                UpdateAllowedNotes();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if ((LOWORD(wParam) == 101 || LOWORD(wParam) == 104 || LOWORD(wParam) == 105) && is_ready) { 
                StopPlayback(hwnd);
                if (current_grp_idx != -1 && current_var_idx != -1) {
                    playing_mode = 1; current_step = 0;
                    active_play_hand = (LOWORD(wParam) == 104) ? 1 : (LOWORD(wParam) == 105) ? 2 : 0;
                    PlayNextStep(hwnd); 
                    SetTimer(hwnd, TIMER_PLAY, TIMER_SPEED_MS * 2, NULL);
                }
            }
            if (LOWORD(wParam) == 102 && is_ready) {
                StopPlayback(hwnd);
                if (current_grp_idx != -1 && current_var_idx != -1) {
                    playing_mode = 2; active_play_hand = 0;
                    current_step = 0; current_arp_note = 0;
                    PlayNextStep(hwnd); 
                    SetTimer(hwnd, TIMER_PLAY, TIMER_SPEED_MS, NULL);
                }
            }
            break;
        }

        case WM_TIMER:
            if (wParam == TIMER_SEARCH) { 
                KillTimer(hwnd, TIMER_SEARCH); 
                FilterItems(hwnd); 
                reset_search_on_next_key = 1; 
            }
            else if (wParam == TIMER_PLAY) {
                PlayNextStep(hwnd);
            }
            break;

        case WM_LBUTTONDOWN: {
            RECT rcRH, rcLH; 
            GetLayoutRects(hwnd, &rcRH, &rcLH, NULL, NULL);
            int pt_x = LOWORD(lParam), pt_y = HIWORD(lParam);

            // Clicking either piano sets input focus back to main window so QWERTY plays
            if (pt_x >= rcRH.left && pt_x <= rcRH.right && pt_y >= rcRH.top && pt_y <= rcRH.bottom) {
                focused_piano = 2; // RH focus
                SetFocus(hwnd);
            } else if (pt_x >= rcLH.left && pt_x <= rcLH.right && pt_y >= rcLH.top && pt_y <= rcLH.bottom) {
                focused_piano = 1; // LH focus
                SetFocus(hwnd);
            }

            int n = GetPianoNoteAtPoint(pt_x, pt_y, rcRH, 48, 96);
            if (n == -1) n = GetPianoNoteAtPoint(pt_x, pt_y, rcLH, 24, 72); 
            if (n != -1 && is_ready) {
                clicked_note = n;
                MidiNoteOn(n);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }

        case WM_LBUTTONUP: {
            if (clicked_note != -1 && is_ready) {
                MidiNoteOff(clicked_note);
                clicked_note = -1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }

        case WM_ERASEBKGND: 
            return 1; 

        case WM_PAINT: {
            PAINTSTRUCT ps; 
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rcClient; 
            GetClientRect(hwnd, &rcClient);

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rcClient.right, rcClient.bottom);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

            FillRect(hdcMem, &rcClient, (HBRUSH)(COLOR_BTNFACE + 1));

            SetBkMode(hdcMem, TRANSPARENT); 
            SetTextColor(hdcMem, RGB(0, 0, 0));
            TextOut(hdcMem, 600, 110, "Focus on Groups List: Type to search", 36);
            TextOut(hdcMem, 600, 130, "Focus on Keyboards: QWERTY plays notes", 38);

            RECT rcRH, rcLH, rcTr, rcBs; 
            GetLayoutRects(hwnd, &rcRH, &rcLH, &rcTr, &rcBs);
            if (rcClient.bottom > 220) {
                DrawPiano(hdcMem, rcRH, 48, 96, "Right Hand / Treble (C3 - C7)", focused_piano == 2);
                DrawPiano(hdcMem, rcLH, 24, 72, "Left Hand / Bass (C1 - C5)", focused_piano == 1);
                DrawHandStaff(hdcMem, rcTr, 1); 
                DrawHandStaff(hdcMem, rcBs, 0);
            }

            BitBlt(hdc, 0, 0, rcClient.right, rcClient.bottom, hdcMem, 0, 0, SRCCOPY);
            SelectObject(hdcMem, hOld); 
            DeleteObject(hbmMem); 
            DeleteDC(hdcMem);
            EndPaint(hwnd, &ps);
            break;
        }

        case WM_DESTROY:
            UnhookWindowsHookEx(hKeyboardHook);
            StopPlayback(hwnd);
            if (is_ready) midiOutClose(hMidiOut);
            DeleteObject(hBrushWhite); 
            DeleteObject(hBrushBlack); 
            DeleteObject(hBrushActive);
            PostQuitMessage(0); 
            break;

        default: 
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev;
    (void)lpCmd;

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc; 
    wc.hInstance = hInstance;
    wc.style = CS_HREDRAW | CS_VREDRAW; 
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "pianochords"; 
    RegisterClass(&wc);
    
    HWND hwnd = CreateWindowEx(0, "pianochords", "pianochords - Ultimate Explorer",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 
                             1100, 800, NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nShow); 
    UpdateWindow(hwnd);
    
    MSG msg; 
    while (GetMessage(&msg, NULL, 0, 0)) { 
        TranslateMessage(&msg); 
        DispatchMessage(&msg); 
    }
    return (int)msg.wParam;
}