#include "audio_engine.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    AudioEngine engine;

    if (!engine.start()) {
        std::cerr << "Failed to start audio engine!" << std::endl;
        return 1;
    }

    std::cout << "Audio engine started." << std::endl;

    // Set some default parameters
    engine.setGain(0.4f);
    engine.setWaveform(WaveformType::Sine);
    engine.setTempo(120.0f);

    // Create a simple sequence: play A4 at beat 0, C#5 at beat 1, E5 at beat 2
    std::vector<SeqEvent> events;
    events.push_back({0.0f, 1, 440.0f});   // A4
    events.push_back({1.0f, 1, 554.37f});  // C#5
    events.push_back({2.0f, 1, 659.25f});  // E5

    engine.setEvents(events);

    // Start playback
    engine.setPlayState(true);

    std::cout << "Playing sequencer... Press ENTER to stop." << std::endl;
    std::cin.get();

    engine.setPlayState(false);
    engine.stop();

    std::cout << "Audio engine stopped." << std::endl;

    return 0;
}
