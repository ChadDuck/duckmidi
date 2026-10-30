extern "C" {
#include "editor_internal.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstring>
#include <cfloat>
static void frame(Editor *e,float x=-100,float y=-100,bool down=false) {
    ImGui::GetIO().AddMousePosEvent(x,y);ImGui::GetIO().AddMouseButtonEvent(0,down);
    int width,height;SDL_GetWindowSize(e->window,&width,&height);ui_frame();editor_frame(e,(float)width,(float)height);ui_render(e->renderer);
}
static ImGuiWindow *child(const char *name) {
    for(auto *w:ImGui::GetCurrentContext()->Windows)if(w->Active&&std::strstr(w->Name,name))return w;
    return nullptr;
}
static void capture(Editor *e,const char *name) {
    SDL_Surface *surface=SDL_RenderReadPixels(e->renderer,nullptr);assert(surface&&SDL_SaveBMP(surface,name));SDL_DestroySurface(surface);
}
static void covered_click(Editor *e,bool *flag,const char *title,float x,float y) {
    *flag=true;frame(e);auto *w=ImGui::FindWindowByName(title);assert(w&&w->Active);
    ImGui::SetWindowPos(w,{x-70,y-60});frame(e,x,y);e->selected_channel=3;e->instrument_channel=-1;
    frame(e,x,y,true);frame(e,x,y,false);
    assert(e->selected_channel==3&&e->instrument_channel==-1);
    if(ImGui::GetCurrentContext()->OpenPopupStack.Size)ImGui::ClosePopupToLevel(0,true);
    *flag=false;frame(e);
}
extern "C" void test_todo_b_ui(Editor *e) {
    assert(e->sidebar==465&&e->midi_split==460);e->focus_pattern=true;frame(e);frame(e);
    auto *playlist=child("/Playlist_");assert(playlist);
    float x=playlist->WorkRect.Min.x+30,y=playlist->WorkRect.Min.y+208;
    e->selected_channel=3;frame(e,x,y);frame(e,x,y,true);frame(e,x,y,false);
    assert(e->selected_channel!=3||e->instrument_channel>=0);e->instrument_channel=-1;frame(e);
    covered_click(e,&e->config,"Project settings",x,y);
    covered_click(e,&e->preferences,"Preferences",x,y);
    covered_click(e,&e->shortcuts,"Keyboard shortcuts",x,y);
    puts("PASS floating configuration/preferences/shortcut windows own overlapping canvas clicks");
    e->context=2;e->edit_focus=FOCUS_NOTES;e->selected_pattern=e->pattern_channel=0;e->project.patterns[0].notes[0].selected=1;
    frame(e);ed_shortcut(e,SDL_SCANCODE_C,SDL_KMOD_ALT,false);assert(e->note_color_popup);
    frame(e);frame(e);auto &stack=ImGui::GetCurrentContext()->OpenPopupStack;assert(stack.Size);auto *popup=stack.back().Window;assert(popup&&popup->Active);
    float left=FLT_MAX,top=FLT_MAX,right=-FLT_MAX,bottom=-FLT_MAX;
    for(const auto &vertex:popup->DrawList->VtxBuffer)if(vertex.col==ed_port_color(2)){left=ImMin(left,vertex.pos.x);right=ImMax(right,vertex.pos.x);top=ImMin(top,vertex.pos.y);bottom=ImMax(bottom,vertex.pos.y);}
    assert(left<right&&top<bottom);float px=(left+right)/2,py=(top+bottom)/2;
    capture(e,"agents/reports/todo-2026-10-29-b/note-colors.bmp");frame(e,px,py);frame(e,px,py,true);frame(e,px,py,false);
    assert(e->project.patterns[0].notes[0].port==2&&ed_port_buttons_width(e)>0);
    int another=dm_pattern_add(&e->project,"Another pattern");assert(another>=0&&dm_clip_add(&e->project,(DmClip){.pattern=another,.channel=0}));e->selected_pattern=another;assert(ed_port_buttons_width(e)>0);e->selected_pattern=0;
    for(size_t i=0;i<e->project.patterns[0].count;i++)e->project.patterns[0].notes[i].selected=1;
    assert(ed_color_notes(e,0)&&ed_port_buttons_width(e)==0);ed_undo(e,false);e->note_ports[0]=0;
    puts("PASS Alt+C mouse popup, palette selection, channel-wide color controls and hiding after removal");
    e->generate_window=true;frame(e);frame(e);auto *window=ImGui::FindWindowByName("Generate Patterns");assert(window&&window->Active&&window->Size.x<=560&&window->Size.y<=230);
    capture(e,"agents/reports/todo-2026-10-29-b/generate-small.bmp");ed_generate_start(e);
    uint64_t start=SDL_GetTicks();while(ed_generate_busy(e)&&SDL_GetTicks()-start<10000){ed_generate_poll(e);SDL_Delay(1);}assert(ed_generate_preview(e));frame(e);frame(e);
    assert(window->Size.x>=900&&window->Size.y>=600);capture(e,"agents/reports/todo-2026-10-29-b/generate-preview.bmp");e->generate_window=false;ed_generate_poll(e);frame(e);
    puts("PASS compact Generate Patterns prompt expands only for the preview");
    e->midi_events=true;frame(e);frame(e);auto *list=child("/MIDI event list_");assert(list&&list->Size.x>=450);e->midi_events=false;frame(e);
    e->shortcuts=true;frame(e);frame(e);auto *shortcuts=ImGui::FindWindowByName("Keyboard shortcuts");assert(shortcuts);auto *rows=child("/Shortcut list_");assert(rows&&rows->ScrollMax.x==0);capture(e,"agents/reports/todo-2026-10-29-b/shortcuts.bmp");e->shortcuts=false;
    frame(e);puts("PASS widened MIDI event list and shortcut content without horizontal overflow");
    e->recent_count=10;for(int i=0;i<10;i++)snprintf(e->recent[i],DM_PATH,"C:/Generated fixtures/Long directory name/very-long-file-name-number-%d.mid",i);
    SDL_SetWindowSize(e->window,640,900);frame(e);frame(e,18,12);frame(e,18,12,true);frame(e,18,12,false);frame(e);
    auto &menus=ImGui::GetCurrentContext()->OpenPopupStack;assert(menus.Size);auto *file=menus[0].Window;assert(file);
    ImGuiID recent=file->GetID("Recent");float mx=file->WorkRect.Min.x+20,my=0;
    for(float probe=file->WorkRect.Min.y;probe<file->WorkRect.Max.y;probe+=3){frame(e,mx,probe);if(ImGui::GetCurrentContext()->HoveredId==recent){my=probe;break;}}
    assert(my);frame(e,mx,my,true);frame(e,mx,my,false);frame(e,mx,my);frame(e,mx,my);
    assert(menus.Size>=2);auto *submenu=menus.back().Window;assert(submenu&&submenu->Active);
    assert(submenu->Pos.x>=file->Pos.x+file->Size.x-5||submenu->Pos.x+submenu->Size.x<=file->Pos.x+5);
    capture(e,"agents/reports/todo-2026-10-29-b/recent-narrow.bmp");ImGui::ClosePopupToLevel(0,true);e->recent_count=0;SDL_SetWindowSize(e->window,1400,900);e->sidebar=465;frame(e);
    puts("PASS Recent submenu stays beside File options in a narrow window");
}
