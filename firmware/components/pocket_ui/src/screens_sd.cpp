#include "pocket/app.hpp"
#include "pocket/ui.hpp"

namespace pocket {

void App::refresh_sd_kind() {
  if (storage_) {
    storage_->probe();
    sd_kind_ = storage_->classify();
  } else {
    sd_kind_ = SdContentKind::Absent;
  }
}

void App::enter_sd_gate_from_hotplug() {
  if (sd_gate_active_) return;
  if (nav_.current() == ScreenId::SdCardGate || nav_.current() == ScreenId::OnboardingSdCard) return;
  sd_return_screen_ = nav_.current();
  sd_waiting_eject_ = false;
  sd_gate_active_ = true;
  refresh_sd_kind();
  focus_.index = 0;
  nav_.push(ScreenId::SdCardGate);
  after_nav();
}

void App::leave_sd_gate() {
  sd_waiting_eject_ = false;
  sd_gate_active_ = false;
  // Pop gate; if stack odd, replace with saved return.
  if (nav_.current() == ScreenId::SdCardGate) {
    if (!nav_.pop()) {
      nav_.reset(sd_return_screen_);
    }
  }
  after_nav();
}

void App::maybe_poll_sd_hotplug() {
  if (!cfg_.onboarding_complete || !storage_) return;
  if (nav_.current() == ScreenId::SdCardGate || nav_.current() == ScreenId::OnboardingSdCard) return;
  if (now_ms_ - last_sd_poll_ms_ < 800) return;
  last_sd_poll_ms_ = now_ms_;

  storage_->probe();
  const bool present = storage_->present();
  if (present && !sd_was_present_ && !sd_gate_active_) {
    play_sound(SoundId::Attention);
    enter_sd_gate_from_hotplug();
    // Gate owns presence until leave; mark seated after successful reformat path.
    return;
  }
  if (!present) {
    sd_was_present_ = false;
  }
}

void App::render_sd_gate() {
  draw_status_bar();
  ui::header(canvas_, "microSD card");
  if (sd_waiting_eject_) {
    ui::empty_state(canvas_, ui::Icon::Sd, "Remove the card", "Waiting for the card to be ejected\xE2\x80\xA6", 380);
    focus_.count = 1;
    ui::button(canvas_, kBottomCtaY, "Checking\xE2\x80\xA6", true);
    return;
  }
  const char* head = "Card detected";
  const char* body = "Reformat prepares the card for music and books. Eject leaves it unused.";
  if (sd_kind_ == SdContentKind::FirmwareRisk) {
    head = "This card looks modified";
    body = "Reformat before Pocket uses it, or eject to keep its files.";
  } else if (sd_kind_ == SdContentKind::Media) {
    head = "Card has media files";
    body = "Reformat clears the card for Pocket. Eject keeps your files.";
  }
  ui::empty_state(canvas_, ui::Icon::Sd, head, body, 360);
  focus_.count = 2;
  ui::row(canvas_, kBottomCtaY - kRowPitch, ui::Row{"Reformat and continue", ui::RowKind::Action}, focus_.index == 0);
  ui::row(canvas_, kBottomCtaY, ui::Row{"Eject", ui::RowKind::Plain}, focus_.index == 1);
}

void App::handle_sd_gate(InputEvent e) {
  if (sd_waiting_eject_) return;
  if (e == InputEvent::Up) {
    focus_.move(-1);
    mark_content_dirty();
    return;
  }
  if (e == InputEvent::Down) {
    focus_.move(1);
    mark_content_dirty();
    return;
  }
  if (e == InputEvent::Back) {
    // Back = eject path without unmount wait if they want to dismiss? Spec: must choose.
    // Treat Back as Eject for calm escape.
    focus_.index = 1;
  }
  if (e != InputEvent::Select && e != InputEvent::Back) return;
  play_sound(SoundId::Click);

  if (focus_.index == 0) {
    if (storage_ && storage_->erase_card()) {
      storage_->unmount();
      storage_->probe();
      storage_->music_ensure_root();
      sd_was_present_ = storage_->present();
      play_sound(SoundId::Success);
      leave_sd_gate();
    } else {
      error_msg_ = "Couldn't reformat the card.";
      error_until_ms_ = now_ms_ + 3000;
      play_sound(SoundId::Attention);
      mark_content_dirty();
    }
    return;
  }

  // Eject
  if (storage_) storage_->unmount();
  sd_waiting_eject_ = true;
  last_sd_poll_ms_ = 0;
  play_sound(SoundId::Attention);
  mark_content_dirty();
}

}  // namespace pocket
