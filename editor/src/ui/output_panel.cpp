#include "ui/output_panel.hpp"

#include "ui/panels.hpp"

#include <cstdarg>
#include <cstdio>

void OutputPanel::info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    this->add(Level::INFO, format, args);
    va_end(args);
}

void OutputPanel::warning(const char* format, ...) {
    va_list args;
    va_start(args, format);
    this->add(Level::WARNING, format, args);
    va_end(args);
}

void OutputPanel::error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    this->add(Level::ERROR, format, args);
    va_end(args);
}

void OutputPanel::clear() {
    this->lines.clear();
}

void OutputPanel::add(const Level level, const char* format, va_list args) {
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    this->lines.push_back({level, buffer});
    this->scroll_to_bottom = this->auto_scroll;
}

void OutputPanel::draw(bool* open) {
    if (!ImGui::Begin(PANELS::OUTPUT, open)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Clear")) {
        this->clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &this->auto_scroll);
    ImGui::SameLine();
    this->filter.Draw("Filter", 200.0f);
    ImGui::Separator();

    if (ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
        for (const Line& line : this->lines) {
            if (!this->filter.PassFilter(line.text.c_str())) {
                continue;
            }
            switch (line.level) {
            case Level::WARNING:
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
                break;
            case Level::ERROR:
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                break;
            case Level::INFO:
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                break;
            }
            ImGui::TextUnformatted(line.text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleVar();

        if (this->scroll_to_bottom || (this->auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
            ImGui::SetScrollHereY(1.0f);
        }
        this->scroll_to_bottom = false;
    }
    ImGui::EndChild();

    ImGui::End();
}
