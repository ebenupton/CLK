//
//  Video.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 10/12/2016.
//  Copyright 2016 Thomas Harte. All rights reserved.
//

#include "Video.hpp"

#include <cstring>

using namespace Electron;

// MARK: - Lifecycle

VideoOutput::VideoOutput(const uint8_t *memory) :
	ram_(memory),
	crt_(h_total,
		1,
		Outputs::Display::Type::PAL50,
		Outputs::Display::InputDataType::Red1Green1Blue1) {
	// Default construction values leave this out of text mode, and text
	// mode uses a subregion of pixel modes.
	crt_.set_fixed_framing([&] {
		run_for(Cycles(10'000));
	});
}

void VideoOutput::set_scan_target(Outputs::Display::ScanTarget *const scan_target) {
	crt_.set_scan_target(scan_target);
}

Outputs::Display::ScanStatus VideoOutput::get_scaled_scan_status() const {
	return crt_.get_scaled_scan_status();
}

void VideoOutput::set_display_type(const Outputs::Display::DisplayType display_type) {
	crt_.set_display_type(display_type);
}

Outputs::Display::DisplayType VideoOutput::get_display_type() const {
	return crt_.get_display_type();
}

// The below is my attempt at transcription of the equivalent VHDL code in moogway82's
// JamSoftElectronULA — https://github.com/moogway82/JamSoftElectronULA — which is itself
// derived from hoglet67's https://github.com/hoglet67/ElectronFpga and that author's
// reverse-engineering of the Electron ULA. It should therefore be as accurate to the
// original hardware as my comprehension of VHDL and adaptation into sequential code allows.

uint8_t VideoOutput::perform(const int h_count, const int v_count) {
	uint8_t interrupts{};

	// In this, the sequential world of C++, all tests below should assume that the position
	// named by (h_count, v_count) is the one that was active **prior to this cycle**.
	//
	// So this cycle spans the period from (h_count, v_count) to (h_count, v_count)+1.

	// Update syncs.
	if(!field_) {
		if(!h_count && v_count == vsync_start) {
			vsync_int_ = true;
		} else if(h_count == h_half && v_count == vsync_end) {
			vsync_int_ = false;
		}
	} else {
		if(h_count == h_half && v_count == vsync_start) {
			vsync_int_ = true;
		} else if(!h_count && v_count == vsync_end + 1) {
			vsync_int_ = false;
		}
	}

	if(h_count == hsync_start) {
		hsync_int_ = true;
	} else if(h_count == hsync_end) {
		hsync_int_ = false;
	}

	// Update character row on the trailing edge of hsync.
	if(h_count == hsync_end) {
		if(is_v_end()) {
			char_row_ = 0;
		} else {
			char_row_ = last_line() ? 0 : char_row_ + 1;
		}
	}

	// Disable the top bit of the char_row counter outside of text mode.
	if(!mode_text_) {
		char_row_ &= 7;
	}

	// Latch video address at frame start.
	if(h_count == h_reset_addr && is_v_end()) {
		row_addr_ = byte_addr_ = screen_base_;
	}

	// Copy byte_addr back into row_addr if a new character row has begun.
	if(hsync_int_) {
		if(last_line()) {
			row_addr_ = row_register_14_6 ? (byte_addr_ & 0x7fc0) : byte_addr_;
		} else {
			byte_addr_ = row_addr_;
		}
	}

	// Determine current output item.
	OutputStage stage;
	int screen_pitch = screen_pitch_;
	if(vsync_int_ || hsync_int_) {
		stage = OutputStage::Sync;
	} else if(in_blank()) {
		if(h_count >= hburst_start && h_count < hburst_end) {
			stage = OutputStage::ColourBurst;
		} else {
			stage = OutputStage::Blank;
		}
	} else {
		stage = OutputStage::Pixels;
		screen_pitch = (mode_40_ ? 320 : 640) / static_cast<int>(mode_bpp_);
	}

	if(stage != output_ || screen_pitch != screen_pitch_) {
		switch(output_) {
			case OutputStage::Sync:			crt_.output_sync(output_length_);					break;
			case OutputStage::Blank:		crt_.output_blank(output_length_);					break;
			case OutputStage::ColourBurst:	crt_.output_default_colour_burst(output_length_);	break;
			case OutputStage::Pixels:
				if(current_output_target_) {
					crt_.output_data(
						output_length_,
						static_cast<size_t>(current_output_target_ - initial_output_target_)
					);
				} else {
					crt_.output_data(output_length_);
				}
			break;
		}
		output_length_ = 0;
		output_ = stage;
		screen_pitch_ = screen_pitch;

		if(stage == OutputStage::Pixels) {
			initial_output_target_ = current_output_target_ = crt_.begin_data(static_cast<size_t>(screen_pitch_));
		}
	}
	output_length_ += 8;
	if(capture && v_count < 313) {
		const int slot = h_count >> 3;
		capture->mode[v_count][slot] = uint8_t(mode_);
		capture->addr[v_count][slot] = 0xffff;
		if(output_ == OutputStage::Pixels && mode_40_) {
			const uint16_t pa = uint16_t(byte_addr_ | char_row_);
			if(ram_[pa]) {
				if(!capture->padl[v_count] && !capture->padr[v_count]) { capture->pad_h[v_count] = uint16_t(h_count); capture->pad_a[v_count] = pa; capture->pad_d[v_count] = ram_[pa]; }
				uint8_t &n = (h_count < 320) ? capture->padl[v_count] : capture->padr[v_count];
				if(n < 255) n++;
			}
		}
		if(output_ != OutputStage::Pixels || (mode_40_ && !(h_count & 8))) {
			for(int i = 0; i < 8; i++) capture->pix[v_count][h_count + i] = 0;
		}
		if(output_ == OutputStage::Pixels && (!mode_40_ || (h_count & 8))) {
			const uint16_t a = uint16_t(byte_addr_ | char_row_);
			const uint8_t d = ram_[a];
			capture->addr[v_count][slot] = a;
			uint8_t *px = &capture->pix[v_count][mode_40_ ? h_count - 8 : h_count];
			const int width = mode_40_ ? 16 : 8;
			switch(mode_bpp_) {
				case Bpp::One:
					for(int i = 0; i < 8; i++)
						for(int j = 0; j < width / 8; j++) px[i * width / 8 + j] = mapped_palette_[((d << i) >> 4) & 8];
				break;
				case Bpp::Two:
					for(int i = 0; i < 4; i++) {
						const uint8_t idx = uint8_t((((d << i) >> 4) & 8) | (((d << i) >> 2) & 2));
						for(int j = 0; j < width / 4; j++) px[i * width / 4 + j] = mapped_palette_[idx];
					}
				break;
				case Bpp::Four:
					for(int i = 0; i < 2; i++) {
						const uint8_t idx = uint8_t((((d << i) >> 4) & 8) | (((d << i) >> 3) & 4) | (((d << i) >> 2) & 2) | (((d << i) >> 1) & 1));
						for(int j = 0; j < width / 2; j++) px[i * width / 2 + j] = mapped_palette_[idx];
					}
				break;
			}
		}
	}
	if(output_ == OutputStage::Pixels && (!mode_40_ || h_count & 8) && current_output_target_) {
		const uint8_t data = ram_[byte_addr_ | char_row_];

		switch(mode_bpp_) {
			case Bpp::One:
				// Maps 1bpp to 4bpp as:
				//
				// 0	->	0000
				// 1	->	1000
				current_output_target_[0] = mapped_palette_[(data >> 4) & 8];
				current_output_target_[1] = mapped_palette_[(data >> 3) & 8];
				current_output_target_[2] = mapped_palette_[(data >> 2) & 8];
				current_output_target_[3] = mapped_palette_[(data >> 1) & 8];
				current_output_target_[4] = mapped_palette_[(data >> 0) & 8];
				current_output_target_[5] = mapped_palette_[(data << 1) & 8];
				current_output_target_[6] = mapped_palette_[(data << 2) & 8];
				current_output_target_[7] = mapped_palette_[(data << 3) & 8];
				current_output_target_ += 8;
			break;
			case Bpp::Two:
				// Maps 2bpp to 4bpp as:
				//
				// 00	->	0000
				// 01	->	1000
				// 10	->	0010
				// 11	->	1010
				current_output_target_[0] = mapped_palette_[((data >> 4) & 8) | ((data >> 2) & 2)];
				current_output_target_[1] = mapped_palette_[((data >> 3) & 8) | ((data >> 1) & 2)];
				current_output_target_[2] = mapped_palette_[((data >> 2) & 8) | ((data >> 0) & 2)];
				current_output_target_[3] = mapped_palette_[((data >> 1) & 8) | ((data << 1) & 2)];
				current_output_target_ += 4;
			break;
			case Bpp::Four:
				current_output_target_[0] =
					mapped_palette_[((data >> 4) & 8) | ((data >> 3) & 4) | ((data >> 2) & 2) | ((data >> 1) & 1)];
				current_output_target_[1] =
					mapped_palette_[((data >> 3) & 8) | ((data >> 2) & 4) | ((data >> 1) & 2) | ((data >> 0) & 1)];
				current_output_target_ += 2;
			break;
		}
	}

	// Increment the byte address across the line.
	// (slghtly pained logic here because the input clock is at the pixel rate, not the byte rate)
	if(h_count < h_active) {
		if(
			(!mode_40_ && !(h_count & 0x7)) ||
			(mode_40_ && ((h_count & 0xf) == (real_m5_phase ? 0x0 : 0x8)))
		) {
			byte_addr_ += 8;

			if(!(byte_addr_ & 0b0111'1000'0000'0000)) {
				byte_addr_ = mode_base_ | (byte_addr_ & 0b0000'0111'1111'1111);	// was a hex literal (elk-headless)
			}
		}
	}

	// Test for interrupts.
	// RTC per the real-chip logic-analyser captures in ElectronFpga/timings (elk-headless):
	// about 15 ticks earlier than the FPGA/JamSoft h=0 and h=512.
	if((!field_ && v_count == v_rtc - 1 && h_count == 1008) || (field_ && v_count == v_rtc && h_count == h_half - 16)) {
		interrupts |= static_cast<uint8_t>(Interrupt::RealTimeClock);
	}
	if(h_count == hsync_start && ((v_count == v_disp_gph && !mode_text_) or (v_count == v_disp_txt && mode_text_))) {
		interrupts |= static_cast<uint8_t>(Interrupt::DisplayEnd);
	}

	return interrupts;
}

uint8_t VideoOutput::run_for(const Cycles cycles) {
	uint8_t interrupts{};

	int number_of_cycles = cycles.as<int>();
	while(number_of_cycles--) {
		interrupts |= perform(h_count_, v_count_);

		// Horizontal and vertical counter updates.
		h_count_ += 8;
		if(h_count_ == h_total) {
			h_count_ = 0;

			if(is_v_end()) {
				if(capture && completed) {
					capture->field = field_;
					*completed = *capture;
					capture->io.clear();
					std::memset(capture->padl, 0, sizeof(capture->padl)); std::memset(capture->padr, 0, sizeof(capture->padr));
				}
				++fields_completed;
				v_count_ = 0;
				field_ = !field_;
			} else {
				++v_count_;
			}
		}
	}

	return interrupts;
}

std::pair<Cycles, uint8_t> VideoOutput::run_until_ram_slot() {
	// Every RAM access is a 1MHz access, stopped and released exactly like IO
	// (FPGA/JamSoft clk_stopped: stop at clken_counter(2..0)=110, release at 1110).
	auto result = run_until_io_slot();		// now h_count_ & 15 == 8

	// In modes 0-3 the release is refused while the line's fetch is active; the
	// release test 2 ticks before this slot lies in the same 8-tick block as
	// h_count_ - 8, which is in the fetch iff !in_blank() at h_count_ (<= 632).
	if(!mode_40_ && !in_blank()) {
		const auto additional = Cycles((h_active + 8 - h_count_) >> 3);	// to h = 648
		result.first += additional;
		result.second |= run_for(additional);
	}
	return result;
}

std::pair<Cycles, uint8_t> VideoOutput::run_until_io_slot() {
	// Two cycles minimum are required; ensure also that the next access
	// ends at the midpoint of a 1Mhz window. In each window the CPU
	// access conceptually comes first.
	const auto duration = 3 - ((h_count_ >> 3) & 1);
	return std::make_pair(duration, run_for(duration));
}

// MARK: - Register hub

void VideoOutput::write(const int address, const uint8_t value) {
	switch(address & 0b1111) {
		case 0x02:
			screen_base_ =
				(screen_base_ & 0b0111'1110'0000'0000) |
				((value << 1) & 0b0000'0001'1100'0000);
		break;
		case 0x03:
			screen_base_ =
				((value << 9) & 0b0111'1110'0000'0000) |
				(screen_base_ & 0b0000'0001'1100'0000);
		break;
		case 0x07: {
			const uint8_t mode = (value >> 3) & 7;
			mode_ = mode;
			mode_40_ = mode >= 4;
			mode_text_ = mode == 3 || mode == 6;

			switch(mode) {
				case 0:
				case 1:
				case 2:		mode_base_ = 0x3000;	break;
				case 3:		mode_base_ = 0x4000;	break;
				case 6:		mode_base_ = 0x6000;	break;
				// Mode value 7 on a real (issue 6) Electron: MODE 4's display, but the address
				// wraps to &3000 (pastraiser.com's hardware notes) (elk-headless).
				case 7:		mode_base_ = real_m5_phase ? 0x3000 : 0x5800;	break;
				default:	mode_base_ = 0x5800;	break;
			}

			switch(mode) {
				default:	mode_bpp_ = Bpp::One;	break;
				case 1:
				case 5:		mode_bpp_ = Bpp::Two;	break;
				case 2:		mode_bpp_ = Bpp::Four;	break;
			}
		} break;
		case 0x08: case 0x09:	set_palette_group<0xfe08, 0b0000>(address, value);	break;
		case 0x0a: case 0x0b:	set_palette_group<0xfe0a, 0b0100>(address, value);	break;
		case 0x0c: case 0x0d:	set_palette_group<0xfe0c, 0b0101>(address, value);	break;
		case 0x0e: case 0x0f:	set_palette_group<0xfe0e, 0b0001>(address, value);	break;
	}
}
