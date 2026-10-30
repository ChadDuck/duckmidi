extern "C" {
#include "editor_export_internal.h"
}
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstring>
static void frame(Editor *e,float x,float y,bool down) {
    auto &io=ImGui::GetIO();io.AddMousePosEvent(x,y);io.AddMouseButtonEvent(0,down);
    ui_frame();editor_frame(e,1000,1000);ui_render(e->renderer);
}
extern "C" void test_export_dropdown(Editor *e,int target) {
    frame(e,-100,-100,false);frame(e,-100,-100,false);
    auto *window=ImGui::FindWindowByName("Export");assert(window&&window->Active);
    ImGuiID id=window->GetID("Export format");float x=window->Pos.x+70,y=0;bool found=false;
    for(float probe=window->WorkRect.Min.y;probe<window->WorkRect.Min.y+160;probe+=4) {
        frame(e,x,probe,false);
        if(ImGui::GetCurrentContext()->HoveredId==id){y=probe;found=true;break;}
    }
    assert(found);frame(e,x,y,true);frame(e,x,y,false);frame(e,x,y,false);
    auto &stack=ImGui::GetCurrentContext()->OpenPopupStack;assert(stack.Size);
    auto *popup=stack.back().Window;assert(popup&&popup->Active);
    SDL_Surface *capture=SDL_RenderReadPixels(e->renderer,nullptr);assert(capture);
    SDL_SaveBMP(capture,"agents/reports/export-plugins/plugin-dropdown.bmp");SDL_DestroySurface(capture);
    float px=popup->WorkRect.Min.x+20;
    int row=target>0?target+1:target==0?0:1;
    float py=popup->DC.CursorStartPos.y+row*ImGui::GetTextLineHeightWithSpacing()+ImGui::GetTextLineHeight()*.5f;
    frame(e,px,py,false);frame(e,px,py,true);frame(e,px,py,false);
    assert(e->export_type==target);
}
extern "C" void test_export_mapping_row(Editor *e,int field_index,int row) {
    frame(e,-100,-100,false);
    ImGuiWindow *sources=nullptr;
    for(auto *w:ImGui::GetCurrentContext()->Windows)if(w->Active&&std::strstr(w->Name,"/sources_"))sources=w;
    assert(sources);
    float x=sources->WorkRect.Min.x+20,y=sources->DC.CursorStartPos.y+row*ImGui::GetTextLineHeightWithSpacing()+ImGui::GetTextLineHeight()*.5f;
    frame(e,x,y,false);frame(e,x,y,true);frame(e,x,y,false);
    auto *format=ed_export_selected(e);assert(format&&format->selected_rows[field_index]==row);
}
extern "C" void test_m64_sequence_dropdown(Editor *e,int target) {
    frame(e,-100,-100,false);frame(e,-100,-100,false);
    auto *w=ImGui::FindWindowByName("Export");assert(w&&w->Active);
    int field=0;ImGuiID scope=ImHashData(&field,sizeof(field),w->IDStack.back());
    ImGuiID id=ImHashStr("Sequence name",0,scope);float x=w->WorkRect.Min.x+70,y=0;bool found=false;
    for(float probe=w->WorkRect.Min.y;probe<w->WorkRect.Min.y+240;probe+=4) {
        frame(e,x,probe,false);if(ImGui::GetCurrentContext()->HoveredId==id){y=probe;found=true;break;}
    }
    assert(found);frame(e,x,y,true);frame(e,x,y,false);frame(e,x,y,false);
    auto &stack=ImGui::GetCurrentContext()->OpenPopupStack;assert(stack.Size);auto *popup=stack.back().Window;
    assert(popup&&popup->Active);
    ImGui::SetScrollY(popup,target==35?popup->ScrollMax.y:0);
    frame(e,x,y,false);frame(e,x,y,false);
    float px=popup->WorkRect.Min.x+20,py=popup->DC.CursorStartPos.y+target*ImGui::GetTextLineHeightWithSpacing()+ImGui::GetTextLineHeight()*.5f;
    assert(py>=popup->InnerRect.Min.y&&py<popup->InnerRect.Max.y);
    frame(e,px,py,false);frame(e,px,py,true);frame(e,px,py,false);frame(e,-100,-100,false);
    auto *format=ed_export_selected(e);DmeUi ui={};assert(format&&format->api->describe(format->instance,&ui)==DME_OK);
    assert(!std::strcmp(ui.fields[0].id,"sequence")&&ui.fields[0].value.integer==target);
    SDL_Surface *capture=SDL_RenderReadPixels(e->renderer,nullptr);assert(capture);
    SDL_SaveBMP(capture,target==35?"agents/reports/m64-export-ui/custom.bmp":"agents/reports/m64-export-ui/preset.bmp");SDL_DestroySurface(capture);
}
extern "C" void test_export_warning(Editor *e) {
    assert(ed_export_warning_open(e));
    frame(e,-100,-100,false);frame(e,-100,-100,false);
    auto *w=ImGui::FindWindowByName("Export warnings");assert(w&&w->Active&&(w->Flags&ImGuiWindowFlags_Modal));
    assert(ImGui::GetCurrentContext()->OpenPopupStack.Size);
    SDL_Surface *capture=SDL_RenderReadPixels(e->renderer,nullptr);assert(capture);
    SDL_SaveBMP(capture,"agents/reports/m64-pitch-range/export-warnings.bmp");SDL_DestroySurface(capture);
    static int close_method=0;
    if((close_method++%3)==1) {
        SDL_Event key={};key.type=SDL_EVENT_KEY_DOWN;key.key.key=SDLK_ESCAPE;key.key.scancode=SDL_SCANCODE_ESCAPE;
        editor_event(e,&key);frame(e,-100,-100,false);
        assert(!ed_export_warning_open(e));return;
    }
    if((close_method%3)==0) {
        float cx=w->Pos.x+w->Size.x-ImGui::GetFrameHeight()*.5f,cy=w->Pos.y+ImGui::GetFrameHeight()*.5f;
        frame(e,cx,cy,false);frame(e,cx,cy,true);frame(e,cx,cy,false);frame(e,cx,cy,false);
        assert(!ed_export_warning_open(e));return;
    }
    float x=w->WorkRect.Min.x+30,y=w->WorkRect.Max.y-20;
    // The OK button follows the scrollable warning body at the bottom.
    bool found=false;ImGuiID id=w->GetID("OK");
    for(float probe=w->WorkRect.Max.y-50;probe<w->WorkRect.Max.y;probe+=3) {
        frame(e,x,probe,false);if(ImGui::GetCurrentContext()->HoveredId==id){y=probe;found=true;break;}
    }
    assert(found);frame(e,x,y,true);frame(e,x,y,false);frame(e,x,y,false);
    assert(!ed_export_warning_open(e));
}
