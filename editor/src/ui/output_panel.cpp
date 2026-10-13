#include "ui/output_panel.hpp"

#include "ui/panels.hpp"

void OutputPanel::sink(const LogRecord& record, void* user_data) {
    static_cast<OutputPanel*>(user_data)->add(record);
}

void OutputPanel::add(const LogRecord& record) {
    Line line;
    line.level = record.level;
    line.category = record.category;
    line.text = record.text;
    line.file = record.file != nullptr ? record.file : "";
    line.line = record.line;
    this->lines.push_back(std::move(line));
    this->scroll_to_bottom = this->auto_scroll;
}

void OutputPanel::clear() {
    this->lines.clear();
}

bool OutputPanel::is_hidden(const LogCategory category) const {
    return category.id < this->hidden.size() && this->hidden[category.id];
}

void OutputPanel::draw_category_menu() {
    if (!ImGui::BeginPopup("categories")) {
        return;
    }
    const u16 count = LOG::category_count();
    if (this->hidden.size() < count) {
        this->hidden.resize(count, false);
    }
    if (ImGui::MenuItem("Show all")) {
        std::fill(this->hidden.begin(), this->hidden.end(), false);
    }
    ImGui::Separator();
    for (u16 i = 0; i < count; ++i) {
        bool shown = !this->hidden[i];
        if (ImGui::MenuItem(LOG::category_name(LogCategory{i}), nullptr, &shown)) {
            this->hidden[i] = !shown;
        }
    }
    ImGui::EndPopup();
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
    if (ImGui::Button("Categories")) {
        ImGui::OpenPopup("categories");
    }
    this->draw_category_menu();
    ImGui::SameLine();
    this->filter.Draw("Filter", 200.0f);
    ImGui::Separator();

    if (ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
        char prefix[640];
        for (const Line& line : this->lines) {
            if (this->is_hidden(line.category)) {
                continue;
            }
            const char* category = LOG::category_name(line.category);
            if (!line.file.empty()) {
                if (line.line != 0) {
                    snprintf(prefix, sizeof(prefix), "[%s] %s:%u: ", category, line.file.c_str(), line.line);
                } else {
                    snprintf(prefix, sizeof(prefix), "[%s] %s: ", category, line.file.c_str());
                }
            } else {
                snprintf(prefix, sizeof(prefix), "[%s] ", category);
            }
            if (!this->filter.PassFilter(line.text.c_str()) && !this->filter.PassFilter(prefix)) {
                continue;
            }
            switch (line.level) {
            case LOG_DEBUG:
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                break;
            case LOG_WARNING:
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
                break;
            case LOG_ERROR:
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                break;
            default:
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                break;
            }
            ImGui::TextUnformatted(prefix);
            ImGui::SameLine(0.0f, 0.0f);
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
