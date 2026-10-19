#include <cassert>
#include <cstring>
#include "nabo_radio_mood.h"
using namespace nabo_radio;

int main() {
    assert(std::strcmp(ActionName(Action::IdleOff), "idle_off") == 0);
    assert(std::strcmp(ActionName(Action::Search), "search") == 0);
    assert(IsOneShot(Action::Love) && !IsOneShot(Action::Groove));
    assert(LinkFromState("Playing") == Link::Playing);
    assert(LinkFromState("Buffering") == Link::Connecting);
    assert(LinkFromState("Reconnecting") == Link::Connecting);
    assert(LinkFromState("Waiting WiFi") == Link::NoNetwork);
    assert(LinkFromState("Unavailable") == Link::Error);
    assert(LinkFromState("Paused") == Link::Stopped);
    assert(LinkFromState("Play") == Link::Stopped);
    assert(LinkFromState(nullptr) == Link::Stopped);

    // Opening the page waves once, then idles.
    Director d;
    Mood mood;
    d.Open(1000);
    assert(d.Update(mood, 1000) == Action::Wave);
    assert(d.Update(mood, 3999) == Action::Wave);
    assert(d.Update(mood, 4000) == Action::IdleOff);

    // Play: phones on, tune while connecting, then listen on a talk station.
    mood.link = Link::Connecting;
    assert(d.Update(mood, 5000) == Action::PhonesOn);
    assert(d.Update(mood, 8000) == Action::Tune);
    mood.link = Link::Playing;
    assert(d.Update(mood, 8100) == Action::Listen);
    mood.music = true;
    assert(d.Update(mood, 12000) == Action::Groove || d.Update(mood, 12000) == Action::Sing);
    const Action a = d.Update(mood, 24000), b = d.Update(mood, 36000);
    assert(a != b && (a == Action::Groove || a == Action::Sing));
    // Dragging the dial while playing shows tuning.
    mood.tuning = true;
    assert(d.Update(mood, 36100) == Action::Tune);
    mood.tuning = false;

    // Reconnecting keeps the phones on; stopping takes them off.
    mood.link = Link::Connecting;
    assert(d.Update(mood, 37000) == Action::Tune);
    mood.link = Link::Playing;
    d.Update(mood, 37100);
    mood.link = Link::Stopped;
    assert(d.Update(mood, 38000) == Action::PhonesOff);
    assert(d.Update(mood, 41000) == Action::IdleOff);

    // A failed station says oops; recovering from an error does not put phones on again.
    mood.link = Link::Error;
    assert(d.Update(mood, 42000) == Action::Oops);
    assert(d.Update(mood, 45000) == Action::IdleOff);
    mood.link = Link::Playing;
    mood.music = false;
    assert(d.Update(mood, 46000) == Action::Listen);

    // A sip arrives 60-120 s into playback and lasts one shot.
    bool sipped = false;
    for (uint32_t t = 46000; t < 46000 + 121000; t += 50) {
        if (d.Update(mood, t) == Action::Sip) {
            assert(t >= 46000 + 60000);
            assert(d.Update(mood, t + 2999) == Action::Sip);
            assert(d.Update(mood, t + 3000) == Action::Listen);
            sipped = true;
            break;
        }
    }
    assert(sipped);

    // Favourite, no network, sleep-timer doze. Shot lengths follow the packs.
    d.SetShotLength(Action::Love, 1500);
    d.Favorite(200000);
    assert(d.Update(mood, 201499) == Action::Love);
    assert(d.Update(mood, 201500) == Action::Listen);
    mood.dozing = true;
    assert(d.Update(mood, 201600) == Action::Doze);
    mood.dozing = false;
    mood.link = Link::NoNetwork;
    assert(d.Update(mood, 201700) == Action::Search);

    // The first observation is a baseline: opening mid-playback does not re-run phones on.
    Director fresh;
    Mood playing;
    playing.link = Link::Playing;
    assert(fresh.Update(playing, 5) == Action::Listen);

    // Reopening re-baselines: a stop that happened while hidden is not replayed,
    // and every shot, even a repeated one, gets a new serial.
    Director reopen;
    Mood live;
    live.link = Link::Playing;
    reopen.Update(live, 1000);
    const uint32_t serial = reopen.ShotSerial();
    reopen.Favorite(2000);
    reopen.Favorite(2100);
    assert(reopen.ShotSerial() == serial + 2);
    Mood stopped;
    reopen.Open(10000);
    assert(reopen.Update(stopped, 10000) == Action::Wave);
    assert(reopen.Update(stopped, 13000) == Action::IdleOff);

    // Times wrap at 2^32 ms without breaking a shot.
    Director wrap;
    wrap.Update(mood, 0xffffff00u);
    wrap.Favorite(0xffffff00u);
    assert(wrap.Update(mood, 0x00000100u) == Action::Love);
    return 0;
}
