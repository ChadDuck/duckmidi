extern "C" {
#include "editor_internal.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstring>
static void frame(Editor *e) {
    ui_frame();editor_frame(e,1400,900);SDL_SetRenderDrawColor(e->renderer,0,0,0,255);SDL_RenderClear(e->renderer);ui_render(e->renderer);SDL_RenderPresent(e->renderer);
}
static void key(Editor *e,ImGuiKey key) {ImGui::GetIO().AddKeyEvent(key,true);frame(e);ImGui::GetIO().AddKeyEvent(key,false);frame(e);}
static void capture(Editor *e,const char *path) {SDL_Surface *surface=SDL_RenderReadPixels(e->renderer,NULL);assert(surface);assert(SDL_SaveBMP(surface,path));SDL_DestroySurface(surface);}
static void replace(Editor *e,const char *text) {
    auto &io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);key(e,ImGuiKey_A);io.AddKeyEvent(ImGuiMod_Ctrl,false);frame(e);io.AddInputCharactersUTF8(text);frame(e);
}
extern "C" void test_todo_c_ui(Editor *e) {
    frame(e);e->rename_pattern=0;std::strcpy(e->rename_text,"Original draft");frame(e);frame(e);
    assert(ui_text_active());replace(e,"Renamed");key(e,ImGuiKey_Enter);
    assert(e->rename_pattern==-1&&!std::strcmp(e->project.patterns[0].name,"Renamed"));
    e->rename_pattern=0;std::strcpy(e->rename_text,"Discard this");frame(e);frame(e);assert(ui_text_active());
    ed_shortcut(e,SDL_SCANCODE_ESCAPE,(SDL_Keymod)0,false);frame(e);assert(e->rename_pattern==-1&&!std::strcmp(e->project.patterns[0].name,"Renamed"));
    puts("PASS Rename auto-focus, Enter submit, and one-press Escape discard while typing");
    ImGuiWindow *group=nullptr,*strip=nullptr;int children=0;
    for(auto *window:ImGui::GetCurrentContext()->Windows)if(window->Active&&std::strstr(window->Name,"/channel group_")&&(!group||window->Pos.x<group->Pos.x))group=window;
    assert(group&&group->Size.x>=120);
    for(auto *window:ImGui::GetCurrentContext()->Windows)if(window->Active&&window->ParentWindow==group&&std::strstr(window->Name,"/port strip_")){children++;if(!strip||window->Pos.x<strip->Pos.x)strip=window;}
    assert(children==2&&strip);
    ImGui::ActivateItemByID(strip->GetID("70"));frame(e);frame(e);assert(ImGui::GetCurrentContext()->OpenPopupStack.Size);
    replace(e,"89");assert(dm_port_mix(&e->project,0,0).volume==70);
    ed_shortcut(e,SDL_SCANCODE_ESCAPE,(SDL_Keymod)0,false);frame(e);assert(dm_port_mix(&e->project,0,0).volume==70);
    ImGui::ActivateItemByID(strip->GetID("70"));frame(e);frame(e);replace(e,"89");key(e,ImGuiKey_Enter);
    assert(dm_port_mix(&e->project,0,0).volume==89&&dm_port_mix(&e->project,1,0).volume==35);
    capture(e,"agents/reports/todo-2026-10-29-c/mixer.bmp");puts("PASS grouped port strips and numeric volume apply/discard");
    e->config=true;e->new_mode=0;frame(e);frame(e);key(e,ImGuiKey_Enter);assert(!e->config);
    e->config=true;frame(e);frame(e);auto *settings=ImGui::FindWindowByName("Project settings");assert(settings&&settings->Size.y<420);
    ImGui::ActivateItemByID(settings->GetID("Name"));frame(e);replace(e,"Settings name");key(e,ImGuiKey_Enter);assert(!e->config);
    puts("PASS compact Project settings and Enter from window/text input");
    e->export_window=true;e->export_type=-1;e->audio_options.codec=DM_AUDIO_MP3;e->audio_options.variable=true;frame(e);frame(e);
    capture(e,"agents/reports/todo-2026-10-29-c/digital-audio.bmp");
    auto *export_window=ImGui::FindWindowByName("Export");assert(export_window&&export_window->Active);
    e->project.patterns[0].length=600;char error[512];
    const char *path="agents/reports/todo-2026-10-29-c/cancel.wav";
    assert(dm_write(path,"sentinel",8,error,sizeof(error)));
    e->audio_options=dm_audio_options_default();
    assert(dm_audio_export(e->audio,&e->project,path,&e->audio_options,error,sizeof(error)));assert(dm_audio_exporting(e->audio));frame(e);
    capture(e,"agents/reports/todo-2026-10-29-c/audio-progress.bmp");dm_audio_cancel(e->audio);
    uint64_t start=SDL_GetTicks();while(dm_audio_busy(e->audio)&&SDL_GetTicks()-start<15000){bool success;int kind;dm_audio_poll(e->audio,error,sizeof(error),&success,&kind);SDL_Delay(1);}
    assert(!dm_audio_exporting(e->audio));DmBytes file={0};assert(dm_read(path,&file,error,sizeof(error))&&file.size==8&&!std::memcmp(file.data,"sentinel",8));dm_bytes_free(&file);frame(e);
    puts("PASS codec controls, render progress visibility, cancellation and destination preservation");
}
