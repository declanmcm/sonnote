// ======= Pannable 2D Canvas (beats x octaves) =======
struct Canvas2D {
    // World-space units
    // x = beats, y = octaves (increasing upward)
    float x0 = 0.0f;       // world x at left edge
    float y0 = 6.0f;       // world y at TOP edge (e.g., show around 6 octaves)
    float sx = 120.0f;     // pixels per beat
    float sy = 80.0f;      // pixels per octave

    bool  snap_to_grid = false;
    bool snap_harmonics = true;
    float snap_x = 0.125f;  // snap to quarter-beats
    float snap_y = 1.0/12.0;  // snap to semitones

    // Stored points in world units (beat, octave)
    std::vector<Note> notes;
    std::unique_ptr<TreeNode> splits = std::make_unique<TreeNode>(-1);
    std::vector<FocusChange> foci;

    bool placing = false;
    ImVec2 anchorW{0,0};
    float split_start = -1.0f;

    Note currently_dragged;
    bool dragging = false;

    bool hover_valid = false;
    ImVec2 hoverW{0,0};

    // Transform: world -> screen
    ImVec2 worldToScreen(ImVec2 topLeft, ImVec2 w) const {
        // y increases upward in world, downward on screen → invert with (y0 - y)
        float sxp = topLeft.x + (w.x - x0) * sx;
        float syp = topLeft.y + (y0 - w.y) * sy;
        return {sxp, syp};
    }
    // Transform: screen -> world
    ImVec2 screenToWorld(ImVec2 topLeft, ImVec2 s) const {
        float wx = x0 + (s.x - topLeft.x) / sx;
        float wy = y0 - (s.y - topLeft.y) / sy;
        return {wx, wy};
    }

    // Round a value to nearest step (for snapping)
    static float snap(float v, float step) {
        return step > 0.f ? std::round(v / step) * step : v;
    }

    std::vector<float> get_bar_points(TreeNode* current, float currentStart, float currentEnd) {
        std::vector<float> list = {currentStart, currentEnd};
        float size = std::abs(currentEnd - currentStart);
        // printf("size: %f\n", size);
        for (const auto& child : current->children) {
            // These are the divisions
            int division = child->value;
            // printf("division: %d\n", division);
            for (int i = 1; i < division; i++) {
                list.push_back(currentStart + ((i * size)/(float)division));
                // printf("point: %f\n", currentStart + ((i * size)/(float)division));
            }

            for (const auto& childChild : child->children) {
                // These are the beat within a division
                int beat = childChild->value;
                // printf("New start: %f, new end: %f\n", currentStart + ((beat * size) / (float)division),  (((beat + 1) * size) /  (float)division));
                std::vector<float> currentList = get_bar_points(childChild.get(), currentStart + ((beat * size) / (float)division), currentStart + (((beat + 1) * size) /  (float)division));
                for (int i = 0; i < (int)currentList.size(); i++) {
                    list.push_back(currentList[i]);
                }
                // printf("\n");
                // std::fflush(stdout);
            }
        }
        return list;
    }

    float snap_split(float v) {
        TreeNode* current_beat = findNode(splits.get(), std::floor(v));
        std::vector<float> points;
        if (!current_beat) {
            points.push_back(std::floor(v));
            points.push_back(std::ceil(v));
        } else {
            points = get_bar_points(current_beat, std::floor(v), std::ceil(v));
        }
        for (int i = 0; i < (int)points.size(); i++) {
            // printf("%f, ", points[i]);
        }
        // printf("\n\n");
        float distance = 9.0;
        float closest = std::floor(v);
        for (int i = 0; i < (int)points.size(); i++) {
            if (std::abs(points[i] - v) < distance) {
                distance = std::abs(points[i] - v);
                closest = points[i];
            }
        }
        return closest;
    }

    void drawRhythmLine(ImVec2 s0, ImDrawList* dl, std::vector<int>& divisions) {
        dl->AddLine({s0.x, s0.y + 10}, {s0.x, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
        if ((int)divisions.size() > 1) {
            switch (divisions.back()) {
                case 8:
                    dl->AddLine({s0.x - 3, s0.y - 16}, {s0.x + 3, s0.y - 16}, IM_COL32(120,120,160,255), 1.0f);
                case 4:
                    dl->AddLine({s0.x - 3, s0.y - 13}, {s0.x + 3, s0.y - 13}, IM_COL32(120,120,160,255), 1.0f);
                case 2:
                    dl->AddLine({s0.x - 3, s0.y - 10}, {s0.x + 3, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 3:
                    dl->AddLine({s0.x - 3, s0.y - 11}, {s0.x + 3, s0.y - 9}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 5:
                    dl->AddLine({s0.x - 3, s0.y - 12}, {s0.x, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
                    dl->AddLine({s0.x + 3, s0.y - 12}, {s0.x, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 7:
                    dl->AddLine({s0.x + 3, s0.y - 10}, {s0.x, s0.y - 12}, IM_COL32(120,120,160,255), 1.0f);
                    dl->AddLine({s0.x - 3, s0.y - 10}, {s0.x, s0.y - 12}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 6:
                    dl->AddLine({s0.x - 3, s0.y - 12}, {s0.x + 3, s0.y - 8}, IM_COL32(120,120,160,255), 1.0f);
                    dl->AddLine({s0.x - 3, s0.y - 8}, {s0.x + 3, s0.y - 12}, IM_COL32(120,120,160,255), 1.0f);
                    break;
            }
        }
    }

    void drawRhythmLines(TreeNode* current, const std::vector<Note>& sortedStart, const std::vector<Note>& sortedEnd, \
        float start, float end, std::vector<int> divisions, std::vector<float>& currently_held, ImVec2 topLeft, ImDrawList* dl) {
        // printf("start\n");
        float beat_size = std::abs(end - start) / divisions.back();
        int latest_starti = 0;
        int latest_endi = 0;
        // printf("start: %f, end: %f, beat_size %f, divisions: %d\n", start, end, beat_size, divisions.back());
        for (int i = 0; i < divisions.back() + 1; i++) {
            float current_beat = start + i * beat_size;
            // printf("current_beat: %f\n", current_beat);
            int starti = latest_starti;
            int endi = latest_endi;
            if ((int)sortedStart.size() > 0 && !((int)divisions.size() > 1 && i == 0) && !((int)divisions.size() > 1 && i == divisions.back())) {
                // printf("endi: %d, sEndsize:%d, sEndBeat_OFF: %f\n", endi, (int)sortedEnd.size(), sortedEnd[endi].beat_off);
                while (sortedStart[starti].beat_on < current_beat)
                    starti++;
                while (sortedEnd[endi].beat_off < current_beat)
                    endi++;
                if ((starti < (int)sortedStart.size() && sortedStart[starti].beat_on == current_beat) || (endi < (int)sortedEnd.size() && sortedEnd[endi].beat_off == current_beat)) {
                    while (starti < (int)sortedStart.size() && sortedStart[starti].beat_on == current_beat) {
                        // printf("adding: %f\n", sortedStart[starti].octave);
                        currently_held.push_back(sortedStart[starti].octave);
                        ImVec2 s0 = worldToScreen(topLeft, { current_beat, sortedStart[starti].octave });
                        drawRhythmLine(s0, dl, divisions);
                        latest_starti = starti;
                        starti++;
                    }
                    while (endi < (int)sortedEnd.size() && sortedEnd[endi].beat_off == current_beat) {
                        std::vector<float> newList;
                        for (int j = 0; j < (int)currently_held.size(); j++) {
                            // printf("diff: %f\n", std::abs(currently_held[j] - sortedEnd[endi].octave));
                            if (std::abs(currently_held[j] - sortedEnd[endi].octave) > 0.0001) {
                                // printf("j: %d\n", j);
                                newList.push_back(currently_held[j]);
                            }
                        }
                        // printf("removing: %f\n", sortedEnd[endi].octave);
                        currently_held = newList;
                        // std::remove(currently_held.begin(), currently_held.end(), sortedEnd[endi].octave);
                        ImVec2 s0 = worldToScreen(topLeft, { current_beat, sortedEnd[endi].octave });
                        drawRhythmLine(s0, dl, divisions);
                        latest_endi = endi;
                        endi++;
                    }
                } else if ((int)currently_held.size() != 0) {
                    float avg = 0.0f;
                    for (int i = 0; i < (int)currently_held.size(); i++) {
                        avg += currently_held[i];
                    }
                    avg = avg / (int)currently_held.size();
                    ImVec2 s0 = worldToScreen(topLeft, { current_beat,  avg});
                    drawRhythmLine(s0, dl, divisions);

                }
                // printf("CH:%d\n", (int)currently_held.size());
            }
            // Check if we have to go deeper on this beat
            for (int j = 0; j < (int)current->children.size(); j++) {
                TreeNode* child = current->children[j].get();
                // printf("val: %d, i: %d\n", child->value, i);
                if (child->value == ((int)divisions.size() == 1 ? i + start : i)) {
                    // printf("match\n");
                    for (const auto& childChild : child->children) {
                        // printf("gotta go inside\n");
                        divisions.push_back(childChild.get()->value);
                        drawRhythmLines(childChild.get(), sortedStart, sortedEnd, start + i * beat_size, start + (i + 1) * beat_size, divisions, currently_held, topLeft, dl);
                        divisions.pop_back();
                    }
                }
            }
        }
        // printf("end\n");
    }

    void draw(const char* label, float height = 300.0f) {
        ImGui::Begin(label);

        // Reserve a canvas rect
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 topLeft  = ImGui::GetCursorScreenPos();
        topLeft.x = topLeft.x + 20;
        ImVec2 size     = { std::max(50.0f, avail.x), std::max(50.0f, avail.y - 50.0f) };
        ImVec2 bottomRight = { topLeft.x + size.x, topLeft.y + size.y };

        // Interaction hitbox
        ImGui::InvisibleButton("canvas_area", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(topLeft, bottomRight, IM_COL32(30,30,30,255));
        dl->AddRect(topLeft, bottomRight, IM_COL32(200,200,200,255));

        // --- Panning with RMB drag ---
        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            // Convert pixel delta → world delta
            x0 -= d.x / sx;
            y0 += d.y / sy; // + because dragging down increases screen y but decreases world y
        }

        // Compute visible world rect
        ImVec2 wA = screenToWorld(topLeft,      topLeft);
        ImVec2 wB = screenToWorld(topLeft,      bottomRight);

        float wxMin = std::min(wA.x, wB.x), wxMax = std::max(wA.x, wB.x);
        float wyMin = std::min(wA.y, wB.y), wyMax = std::max(wA.y, wB.y);

        // --- Grid lines (integer beats/octaves) ---
        const ImU32 gridCol   = IM_COL32(60,60,60,255);
        const ImU32 axisCol   = IM_COL32(120,120,160,255);
        const ImU32 textCol   = IM_COL32(220,220,220,255);
        const ImU32 playheadCol = IM_COL32(200, 20, 20, 255);
        const ImU32 noteCol = IM_COL32(90, 160, 255, 255);
        const ImU32 focusCol = IM_COL32(255,160,90,200);

        // Sort notes
        std::vector<Note> sorted = notes;
        std::vector<Note> sortedEnd = notes;
        std::vector<float> currently_held;
        float end = 0.0f;
        std::sort(sorted.begin(), sorted.end(),
                [](auto& a, auto& b){ 
                    return a.beat_on < b.beat_on; });
        std::sort(sortedEnd.begin(), sortedEnd.end(),
                [](auto&a, auto& b){
                    return a.beat_off < b.beat_off;});
        if ((int)sortedEnd.size() > 0) {
            end = sortedEnd[(int)sortedEnd.size() - 1].beat_off;
        }

        // Vertical: beats at integer positions within [wA.x, wB.x]
        int beatStart = std::max(std::floor(wA.x), (int)sorted.size() > 0 ? std::floor(sorted[0].beat_on) : -99999.0f);
        int beatEnd   = std::min(std::ceil (wB.x), std::ceil(end));
        std::vector<int> divisions;
        divisions.push_back(std::abs(beatEnd - beatStart));
        drawRhythmLines(splits.get(), sorted, sortedEnd, beatStart, beatEnd, divisions, currently_held, topLeft, dl);

        // Horizontal: octaves at integer positions
        int octStart = (int)std::floor(wB.y);
        int octEnd   = (int)std::ceil (wA.y);

        // Draw playhead
        ImVec2 s0 = worldToScreen(topLeft, { playhead_time, wA.y });
        dl->AddLine({s0.x, topLeft.y}, {s0.x, bottomRight.y}, playheadCol);

        // Hover snapped position (for preview dot / rubber band)
        hover_valid = false;
        if (hovered) {
            ImVec2 s = ImGui::GetIO().MousePos;
            ImVec2 w = screenToWorld(topLeft, s);
            std::fflush(stdout);
            w.x = snap_split(w.x);
            if (snap_to_grid) {
                w.y = snap(w.y, snap_y);
            } else if (snap_harmonics) {
                float focus = 0.0f;
                for (int i = 0; i < (int)foci.size() - 1; i++) {
                    if (w.x > foci[i].time && w.x < foci[i + 1].time) {
                        focus = foci[i].octave;
                        break;
                    }
                }

                float currentOctave = focus;
                if (currentOctave > w.y) {
                    while (currentOctave > w.y)
                        currentOctave -= 1;
                } else if (currentOctave + 1 < w.y) {
                    while (currentOctave + 1 < w.y) 
                        currentOctave += 1;
                }

                // printf("Y pos: %f\n", w.y);
                float closest = currentOctave;
                for (int i = 0; i < (int)sizeof(perfectIntervals) / 4; i++) {
                    // printf("Current: %f\n", currentOctave + std::log2(perfectIntervals[i]));
                    // printf("Distance: %f\n", std::abs(w.y - (currentOctave + std::log2(perfectIntervals[i]))));
                    // printf("Distance to closest: %f\n", std::abs(w.y - closest));
                    if (std::abs(w.y - (currentOctave + std::log2(perfectIntervals[i]))) < std::abs(w.y - closest)) {
                        // printf("YO\n");
                        closest = currentOctave + std::log2(perfectIntervals[i]);
                    }
                }
                w.y = closest;
            }
            // printf("w.x: %f, w.y: %f\n", w.x, w.y);
            std::fflush(stdout);
            hoverW = w;
            hover_valid = true;
        }

        // Key handling (P to change playhead position, L to change note length, D to delete a note, F to change the note's frequency)
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_P)) 
            playhead_time = hoverW.x;
        else if (hovered && ImGui::IsKeyPressed(ImGuiKey_Space))
            play_triggered_from_space = true;

        if (hovered && (ImGui::IsKeyPressed(ImGuiKey_L) || ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_S))) {
            // Find closest note and side of note (start or end)
            if ((int)notes.size() > 0) {
                Note closest = notes[0];
                float distance =  99999;
                int index = 0;
                bool match_found = false;
                for (int i = 0; i < (int)notes.size(); i++) { 
                    float current = std::abs(notes[i].octave - hoverW.y);
                    if ((hoverW.x <= notes[i].beat_off && hoverW.x  >= notes[i].beat_on) && current < distance) {
                        match_found = true;
                        distance = current;
                        closest = notes[i];
                        index = i;
                    } 
                }
                if (match_found) {
                    // Drag start or end of note
                    if (ImGui::IsKeyPressed(ImGuiKey_L)) {
                        anchorW.y = closest.octave;
                        if (std::abs(hoverW.x - closest.beat_off) < std::abs(hoverW.x - closest.beat_on)) {
                            anchorW.x = closest.beat_on;
                        } else {
                            anchorW.x = closest.beat_off;
                        }
                        placing = true;
                    }

                    if (!(ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_S))) {
                        // Delete the note because it will be replaced
                        notes.erase(notes.begin() + index);
                    } else {
                        if (ImGui::IsKeyPressed(ImGuiKey_J)) {
                            // Jump up by the written amount
                            notes[index].octave = notes[index].octave + (std::log2(parseDivisor(jump)));
                        } else if (ImGui::IsKeyPressed(ImGuiKey_S)) {
                            std::fflush(stdout);
                            if (split_start != -1.0f) {
                                float split_end = hoverW.x;
                                if ((split_end > split_start) && std::ceil(split_end) - std::floor(split_start) == 1) {
                                    // Split time into specified amount
                                    int current_beat = std::floor(split_start);
                                    TreeNode* beat_node = findNode(splits.get(), current_beat);
                                    if (!beat_node) {
                                        // Create entry for that beat
                                        splits.get()->addChild(current_beat);
                                    }
                                    find_division(findNode(splits.get(), current_beat), parseDivisor(split), current_beat, current_beat + 1.0f, split_start, split_end);
                                    printTree(splits.get());
                                }
                                split_start = -1.0f;
                            } else {
                                split_start = hoverW.x;
                            }
                        }
                    }

                    // Frequency drag
                    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                        dragging = true;
                        currently_dragged = closest;
                    }
                    


                    // Delete the focus if the note had focus on it
                    std::vector<FocusChange> newFoci;
                    int count = 0;
                    for (int i = 0; i < (int)foci.size(); i++) {
                        // printf("i: %d\n", i);
                        if (foci[i].time == closest.beat_on) {
                            if (foci[i].octave != closest.octave) {
                                newFoci.push_back(foci[i]);
                            } else if (i != 0 && foci[i - 1].octave != 0.0f) {
                                newFoci.push_back(foci[i]);
                                newFoci[count].octave = 0.0f;
                                count++;
                            }
                        } else if (foci[i].time > closest.beat_on && foci[i].time < closest.beat_off) {
                            if (foci[i].octave == 0.0f && foci[i - 1].octave != closest.octave) {
                                newFoci.push_back(foci[i]);
                                count++;
                            } else if (foci[i].octave != closest.octave) {
                                newFoci.push_back(foci[i]);
                                count++;
                            } else if (foci[i].octave == closest.octave) {
                                newFoci.push_back(foci[i]);
                                newFoci[count].octave = 0.0f;
                                count++;
                            }
                        } else if (foci[i].time == closest.beat_off) {
                            // printf("!!!!!!!!\n");
                            if ((i != 0 && foci[i - 1].octave != closest.octave) || ((i != (int)foci.size() - 1) && foci[i].octave != 0.0f)) {
                                // printf("??????\n");
                                newFoci.push_back(foci[i]);
                                count++;
                            }
                        } else {
                            // printf("wot");
                            newFoci.push_back(foci[i]);
                            count++;
                        }
                    }
                    // printf("foci: \n");
                    for (int i = 0; i < (int)foci.size(); i++) {
                        // printf("i: %d, ", i);
                        // printf("octave: %f, ", foci[i].octave);
                        // printf("time: %f, ", foci[i].time);
                    }
                    foci = newFoci;
                    // printf("newFoci: \n");
                    for (int i = 0; i < (int)foci.size(); i++) {
                        // printf("i: %d, ", i);
                        // printf("octave: %f, ", foci[i].octave);
                        // printf("time: %f, ", foci[i].time);
                    }
                }
            }
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (!placing) {
                // Find closest note
                std::vector<Note> within;
                for (const Note& n : notes) {
                    if (hoverW.x <= n.beat_off && hoverW.x  >= n.beat_on)
                        within.push_back(n);
                }
                if ((int)within.size() > 0) {
                    Note closest = within[0];
                    float distance = 999999;
                    for (const Note& n : within) {
                        float current = std::abs(n.octave - hoverW.y);
                        if (current < distance) {
                            distance = current;
                            closest = n;
                        }
                    }
                    // Add to focus changes as far left and as far right as we can
                    int insert = 0;
                    bool focus_after = true;
                    bool focus_before = true;
                    std::vector<FocusChange> newFoci;
                    for (int i = 0; i < (int)foci.size(); i++) {
                        newFoci.push_back(foci[i]);
                        if (foci[i].time > hoverW.x) {
                            // This index is the next focus change after the click point
                            insert = i;
                            if (foci[i].octave == 0.0f)
                                focus_after = false;
                            else if (i <= 1 || foci[i - 2].octave == 0.0f)
                                focus_before = false;
                            break;
                        } else {
                            if (i == (int)foci.size() - 1) {
                                insert = i + 1;
                            }
                        }
                    }
                    // printf("insert: %d\n", insert);
                    // printf("focus_before: %d, focus_after: %d\n", focus_before, focus_after);
                    std::fflush(stdout);
                    bool removePrevious = false;
                    bool removeNext = false;
                    float previousOctave = 0.0f;
                    if (insert > 0)
                        previousOctave = foci[insert - 1].octave;
                    FocusChange start, end;
                    bool extending_focus = closest.octave == foci[insert - 2].octave;
                    bool extend_to_note_start = insert == 0 || closest.beat_on > foci[insert - 1].time;
                    bool start_coincides_with_other_start = insert > 0 && closest.beat_on == foci[insert - 1].time;
                    if (start_coincides_with_other_start) {
                        newFoci[insert - 1].time = closest.beat_on;
                        newFoci[insert - 1].octave = closest.octave;
                    } else if (extend_to_note_start) {
                        // printf("to_start\n");
                        std::fflush(stdout);
                        // The note the user clicked on starts after than the previous focus change
                        start.time = closest.beat_on;
                        start.octave = closest.octave;
                    } else if (!focus_before) {
                        // There is a period of rest in focus before the previous focus change
                        // Nonetheless we must check if there is another focus change that happens after the note starts
                        if (insert > 1 && closest.beat_on < foci[insert - 2].time) {
                            // Change insert - 2 to be the current frequency instead of zero, then delete insert - 1
                            newFoci[insert - 2].octave = closest.octave;
                            removePrevious = true;
                        } else {
                            newFoci[insert - 1].time = closest.beat_on;
                            newFoci[insert - 1].octave = closest.octave;
                        }
                    } else {
                        // The note starts before the previous focus change and there is another focus immediately before it (or it is the first focus)
                        newFoci[insert - 1].octave = closest.octave;
                    }

                    bool extend_to_note_end = (int)foci.size() == 0 || insert == (int)foci.size() || insert != (int)foci.size() && closest.beat_off < foci[insert].time;
                    bool end_coincides_with_other_end = insert <= (int)foci.size() - 1 && closest.beat_off == foci[insert].time;
                    if (end_coincides_with_other_end) {
                        newFoci[insert].time = closest.beat_off;
                        newFoci[insert].octave = previousOctave;
                    } else if (extend_to_note_end) {
                        // The note ends before the next focus change
                        end.time = closest.beat_off;
                        end.octave = previousOctave;
                        // printf("prev: %f\n", previousOctave);
                        std::fflush(stdout);
                    } else if (!focus_after) {
                        // Still must check if the one after the next happens before the end of the note
                        if (insert == (int)foci.size() || foci[insert + 1].time < closest.beat_off) {
                            removeNext = true;
                        } else {
                            newFoci[insert].time = closest.beat_off;
                            newFoci[insert].octave = 0.0f;
                        }
                    }

                    if (removePrevious)
                        newFoci.erase(newFoci.begin() + (insert - 1));
                    if (removeNext)
                        newFoci.erase(newFoci.begin() + insert);

                    if (extend_to_note_start && extend_to_note_end) {
                        newFoci.insert(newFoci.begin() + insert, start);
                        newFoci.insert(newFoci.begin() + insert + 1, end);
                    } else if (extend_to_note_start) {
                        newFoci.insert(newFoci.begin() + insert, start);
                    } else if (extend_to_note_end) {
                        newFoci.insert(newFoci.begin() + insert, end);
                    }

                    for (int i = insert + 1; i < (int)foci.size(); i++)
                        newFoci.push_back(foci[i]);

                    std::vector<FocusChange> newNewFoci;

                    for (int i = 0; i < (int)newFoci.size(); i++) {
                        if (i == 0 || newFoci[i].octave != newFoci[i - 1].octave) {
                            newNewFoci.push_back(newFoci[i]);
                        }
                    }

                    foci = newNewFoci;
                    for (int i = 0; i < (int)foci.size(); i++) {
                        // printf("Focus %d: time: %f, octave: %f\n", i, foci[i].time, foci[i].octave);
                    }
                }
            }
        }
        

        // Existing notes as blue lines
        int focusIndex = 0;
        Note closest;
        float distance = 9999.0f;
        for (const Note& n : notes) {
            float x0w = n.beat_on, x1w = n.beat_off;
            float y   = n.octave;
            // Calculate distance
            float current_distance = 0.0f;
            if (hoverW.x >= n.beat_on && hoverW.x <= n.beat_off) {
                current_distance = std::abs(y - hoverW.y);
            } else if (hoverW.x < n.beat_on) {
                current_distance = std::sqrt(std::pow(2, hoverW.x - n.beat_on) + std::pow(2, hoverW.y - y));
            } else {
                current_distance = std::sqrt(std::pow(2, hoverW.x - n.beat_off) + std::pow(2, hoverW.y - y));
            }
            if (current_distance < distance) {
                closest = n;
                distance = current_distance;
            }
            // rectangle from (x0w,y±h/2) to (x1w,y±h/2); choose fixed  (e.g., 0.6 octave)
            // float halfH = 1.0/24.0;
            ImVec2 a = worldToScreen(topLeft, { x0w, y});
            ImVec2 b = worldToScreen(topLeft, { x1w, y});
            // // ensure correct corners
            ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            dl->AddRectFilled(rmin, rmax, IM_COL32(90,160,255,200), 2.0f);

            float tri_size = 6.0f;
            ImVec2 p1(a.x + tri_size, a.y);                       // tip of triangle
            ImVec2 p2(a.x, a.y - tri_size / 2); // top base corner
            ImVec2 p3(a.x, a.y); // bottom base corner

            dl->AddTriangleFilled(p1, p2, p3, noteCol);
        }

        // Add an origin line for the closest note
        if (closest.origin != 0.0f) {
            ImVec2 a = worldToScreen(topLeft, { closest.beat_on, closest.octave});
            ImVec2 c = worldToScreen(topLeft, {closest.beat_on, closest.origin});
            dl->AddRectFilled(a, c, IM_COL32(90,255,90,200), 2.0f);
        }

        // Draw focus areas on top of existing notes
        for (int i = 0; i < (int)foci.size() - 1; i++) {
            if (foci[i].octave == 0.0f) {
                continue;
            }
            float x0w = foci[i].time;
            float x1w = foci[i + 1].time;
            float y = foci[i].octave;
            float halfH = 1.0 / 24.0;
            ImVec2 a = worldToScreen(topLeft, { x0w, y });
            ImVec2 b = worldToScreen(topLeft, { x1w, y });
            // ensure correct corners
            ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            dl->AddRectFilled(rmin, rmax, IM_COL32(255,160,90,200), 2.0f);
            // dl->AddRect(rmin, rmax, IM_COL32(100,60,30,220), 6.0f, 0, 2.0f);

            // float tri_size = 6.0f;
            // ImVec2 p1(a.x + tri_size, a.y);                       // tip of triangle
            // ImVec2 p2(a.x, a.y - tri_size / 2); // top base corner
            // ImVec2 p3(a.x, a.y); // bottom base corner

            // dl->AddTriangleFilled(p1, p2, p3, focusCol);

            // dl->AddLine({a.x, a.y}, {b.x, a.y}, focusCol);

            if (snap_harmonics) {
                float startOctave = foci[i].octave;
                float endOctave = foci[i].octave;
                if (startOctave < octStart) {
                    while ((startOctave + 1) < octStart) {
                        startOctave += 1;
                    }
                }
                while (startOctave > octStart) {
                    startOctave -= 1;
                }
                if (endOctave > octEnd) {
                    while ((endOctave - 1) > octEnd) {
                        endOctave -= 1;
                    }
                }
                while (endOctave < octEnd) {
                    endOctave += 1;
                }
                for (float o = startOctave; o <= endOctave; o++) {
                    if (o != foci[i].octave) {
                        ImVec2 s0 = worldToScreen(topLeft, { wB.x, o});
                        dl->AddLine({rmin.x, s0.y}, {rmax.x, s0.y}, gridCol);
                    }
                    for (int i = 0; i < sizeof(perfectIntervals) / 4; i++) {
                        float currentOctave = o + std::log2(perfectIntervals[i]);
                        if (currentOctave > octStart && currentOctave < octEnd) {
                            ImVec2 s0 = worldToScreen(topLeft, { wB.x, currentOctave});
                            dl->AddLine({rmin.x, s0.y}, {rmax.x, s0.y}, gridCol);
                        }
                    }
                }
                std::fflush(stdout);
            }
        }

        // Rubber-band preview (after first click)
        if (placing && hover_valid) {
            float x0w = anchorW.x;
            float x1w = hoverW.x;
            float y   = anchorW.y; // octave fixed from first click
            float halfH = 1.0/24.0;
            ImVec2 a = worldToScreen(topLeft, { x0w, y });
            ImVec2 b = worldToScreen(topLeft, { x1w, y });

            // dl->AddLine({a.x, a.y}, {b.x, a.y}, noteCol);
            ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            dl->AddRectFilled(rmin, rmax, IM_COL32(120,120,255,120), 2.0f);
            // dl->AddRect(rmin, rmax, IM_COL32(80,80,200,180), 6.0f, 0, 2.0f);
        }

        // Light-red preview dot (when Snap is on and hovered)
        if ((snap_to_grid || snap_harmonics) && hover_valid) {
            ImVec2 sd = worldToScreen(topLeft, hoverW);
            dl->AddCircleFilled(sd, 5.0f, IM_COL32(255,120,120,200));
            if (dragging) {
                Note n = currently_dragged;
                float x0w = n.beat_on, x1w = n.beat_off;
                float y   = hoverW.y;
                // rectangle from (x0w,y±h/2) to (x1w,y±h/2); choose fixed height (e.g., 0.6 octave)
                float halfH = 1.0/24.0;
                ImVec2 a = worldToScreen(topLeft, { x0w, y + halfH });
                ImVec2 b = worldToScreen(topLeft, { x1w, y - halfH });
                // ensure correct corners
                ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
                ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
                dl->AddRectFilled(rmin, rmax, IM_COL32(90,160,255,200), 6.0f);
                dl->AddRect(rmin, rmax, IM_COL32(30,60,100,220), 6.0f, 0, 2.0f);
            }
        }

        // Input: left-click to place/finish
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (!placing) {
                // First click: start note (snap already applied above)
                if (hover_valid) {
                    if (dragging) {
                        currently_dragged.octave = hoverW.y;
                        notes.push_back(currently_dragged);
                        dragging = false;
                    } else {
                        placing = true;
                        anchorW = hoverW;
                    }
                }
            } else {
                // Second click: finish note
                if (hover_valid) {
                    float a = anchorW.x, b = hoverW.x;
                    if (a != b) {
                        Note n;
                        n.beat_on  = std::min(a, b);
                        n.beat_off = std::max(a, b);   // exclusive
                        n.octave   = anchorW.y;        // octave fixed at first click
                        n.origin = 0.0f;
                        if (snap_harmonics) {
                            // Check whether there's any focus at the anchor
                            for (int i = 1; i < (int)foci.size(); i++) {
                                if (i == 0) 
                                    continue;

                                // printf("foci[i - 1].time: %f, anchorW.x: %f, foci[i].time: %f\n", foci[i - 1].time, anchorW.x, foci[i].time);
                                
                                if (foci[i - 1].time <= anchorW.x && foci[i].time >= anchorW.x) {
                                    n.origin = foci[i - 1].octave;
                                    break;
                                }
                            }
                        }
                        notes.push_back(n);
                    }
                }
                placing = false;
            }
        }

        // HUD: controls + current mouse coords in world units
        ImVec2 mouse = ImGui::GetIO().MousePos;
        if (hovered) {
            ImVec2 w = screenToWorld(topLeft, mouse);
            ImGui::SetCursorScreenPos({topLeft.x + 8, bottomRight.y - 50});
            ImGui::BeginGroup();
            ImGui::Text("Mouse: beat %.3f, oct %.3f", w.x, w.y);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputFloat("dx", &snap_x, 0.0f, 0.0f, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputFloat("dy", &snap_y, 0.0f, 0.0f, "%.2f");
            ImGui::EndGroup();
        } else {
            // Put the cursor back to layout after InvisibleButton
            ImGui::Dummy(ImVec2(0,0));
        }

        ImGui::Checkbox("Snap", &snap_to_grid);
        ImGui::Checkbox("Harmonic Snap", &snap_harmonics);

        ImGui::End();
    }
};



    // divisions holds the list of recursive split sizes
    void drawRhythmLines(TreeNode* current, const std::vector<Note>& sortedStart, const std::vector<Note>& sortedEnd, \
        float start, float end, std::vector<int> divisions, std::vector<float>& currently_held, VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight) {
        // printf("start\n");
        float beat_size = std::abs(end - start) / divisions.back();
        int latest_starti = 0;
        int latest_endi = 0;
        // printf("start: %f, end: %f, beat_size %f, divisions: %d\n", start, end, beat_size, divisions.back());
        // Currently dealing with the last-appended number to divisions - the current number to split into
        for (int i = 0; i < divisions.back() + 1; i++) {
            float current_beat = start + i * beat_size;
            int starti = latest_starti;
            int endi = latest_endi;
            // Only draw a line if there is a note playing, keep track of playing notes to perform averages
            // Only draw lines in the middles if these are subdivisions
            if ((int)sortedStart.size() > 0 && ((int)divisions.size() == 1 || (i != 0 && i != divisions.back()))) {
                // printf("endi: %d, sEndsize:%d, sEndBeat_OFF: %f\n", endi, (int)sortedEnd.size(), sortedEnd[endi].beat_off);
                if (starti >= (int)sortedStart.size()) {
                    printf("ISSUE: starti: %d, latest_starti: %d\n", starti, latest_starti);
                }
                // Setup start and end indexes to line up with the next note event we'll encounter (purely an optimisation)
                while (starti < (int)sortedStart.size() && sortedStart[starti].beat_on < current_beat)
                    starti++;
                while (endi < (int)sortedEnd.size() && sortedEnd[endi].beat_off < current_beat)
                    endi++;
                // If this is true, a note is starting or ending on current_beat, we must keep track of this for the average
                if ((starti < (int)sortedStart.size() && std::abs(sortedStart[starti].beat_on - current_beat) < 1e-6) || (endi < (int)sortedEnd.size() && std::abs(sortedEnd[endi].beat_off - current_beat) < 1e-6)) {
                    while (starti < (int)sortedStart.size() && std::abs(sortedStart[starti].beat_on - current_beat) < 1e-6) {
                        currently_held.push_back(sortedStart[starti].octave);
                        Vec2 s0 = worldToScreen(topLeft, { current_beat, sortedStart[starti].octave });
                        drawRhythmLine(s0, renderer, divisions, topLeft, bottomRight);
                        latest_starti = starti;
                        starti++;
                    }
                    while (endi < (int)sortedEnd.size() && std::abs(sortedEnd[endi].beat_off - current_beat) < 1e-6) {
                        std::vector<float> newList;
                        for (int j = 0; j < (int)currently_held.size(); j++) {
                            // printf("diff: %f\n", std::abs(currently_held[j] - sortedEnd[endi].octave));
                            if (std::abs(currently_held[j] - sortedEnd[endi].octave) > 0.0001) {
                                // printf("j: %d\n", j);
                                newList.push_back(currently_held[j]);
                            }
                        }
                        // printf("removing: %f\n", sortedEnd[endi].octave);
                        currently_held = newList;
                        // std::remove(currently_held.begin(), currently_held.end(), sortedEnd[endi].octave);
                        Vec2 s0 = worldToScreen(topLeft, { current_beat, sortedEnd[endi].octave });
                        drawRhythmLine(s0, renderer, divisions, topLeft, bottomRight);
                        latest_endi = endi;
                        endi++;
                    }
                } else if ((int)currently_held.size() != 0) {
                    // If there are notes held but no note events occur on current_beat, take an average and place the marker between them
                    float avg = 0.0f;
                    for (int i = 0; i < (int)currently_held.size(); i++) {
                        avg += currently_held[i];
                    }
                    avg = avg / (int)currently_held.size();
                    Vec2 s0 = worldToScreen(topLeft, { current_beat,  avg});
                    drawRhythmLine(s0, renderer, divisions, topLeft, bottomRight);

                }
                // printf("CH:%d\n", (int)currently_held.size());
            }
            // Check if we have to go deeper on this beat
            for (int j = 0; j < (int)current->children.size(); j++) {
                TreeNode* child = current->children[j].get();
                // printf("val: %d, i: %d\n", child->value, i);
                if (child->value == ((int)divisions.size() == 1 ? i + start : i)) {
                    // printf("match\n");
                    for (const auto& childChild : child->children) {
                        // printf("gotta go inside\n");
                        divisions.push_back(childChild.get()->value);
                        drawRhythmLines(childChild.get(), sortedStart, sortedEnd, start + i * beat_size, start + (i + 1) * beat_size, divisions, currently_held, renderer, topLeft, bottomRight);
                        divisions.pop_back();
                    }
                }
            }
        }
        // printf("end\n");
    }
