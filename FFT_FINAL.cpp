#include <iostream> 
#include <cmath> 
#include <string> 
#include <vector>
#include <complex>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"

using namespace std;

//CONSTANTS//
const float PI_F = 3.1415926535f;
const int N = 16384; //sample size
const float fs = 62500.0f; //sampling frequency
const int total_blocks = 12; //Defines how many fft sample blocks will be recorded before playback.
//12 blocks of 16384 samples at 62.5kHz is about 3.15 seconds of audio.
const int audiobuffer_size = N * total_blocks; //buffer size to hold all recorded audio samples for playback
//END OF CONSTANTS//


//store N samples as complex numbers
vector<complex<float>> samples(N);

//Buffer to hold audio data for playback
uint8_t audio_playback_buffer[audiobuffer_size];
int audio_record_pointer = 0;
//variables for sampling (volatile since they are used inside and outside interrupt context)
volatile int sample_index = 0;
volatile bool rdy_block = false;



//Audio sampling useing the pico 2's ADC. interrupt handled at 16us (corresponds to sampling frequency).
bool timer_callback(struct repeating_timer *t) {
    if (sample_index < N) {//collect N samples for current block
        uint16_t raw = adc_read(); //read raw analogue value from microphone connected to pico 2.
        //^^convert raw adc value to digital value using the pico 2's adc.
        //convert adc output to float in range -1.0 to 1.0. Midpoint is 2048 for 12 bit adc.
        float f_value = ((float)raw - 2048.0f) / 2048.0f;
        //store the sample for the fft as a complex number, ready to be processed.
        samples[sample_index] = complex<float>(f_value, 0.0f);
        
        //sample is then stored ready for playback.
        //8 bit unsigned int, required val for pwm audio output.
        if (audio_record_pointer < audiobuffer_size) {
            audio_playback_buffer[audio_record_pointer++] = (uint8_t)((f_value + 1.0f) * 127.5f);
        }
        sample_index++;
    } 
    //tell timer when to stop sampling.
    else 
    {
    rdy_block = true; //when timer is done sampling.
    }
    return !rdy_block;
}
//end of timer callback func

//Note names. Lookup table
const string notenames[]={"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void play_audio(uint gpio_pin, float pitch_multiplier) {
    // Calculate delay based on the pitch scale (default is 16us), correspond to fs
    int delay_us = (int)(16.0f / pitch_multiplier);
    if (delay_us < 1) delay_us = 1;
    //configure GPIO pin for PWM speaker output. One of pico 2's dedicated PWM pin.
    gpio_set_function(gpio_pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(gpio_pin);
    pwm_set_wrap(slice_num, 255);
    pwm_set_enabled(slice_num, true);

    //output samples sequentially
    //samples cntrol PWM duty cycle.
    for (int i = 0; i < audiobuffer_size; i++) {
        pwm_set_gpio_level(gpio_pin, audio_playback_buffer[i]);
        sleep_us(delay_us); 
    } 
    //stop pwm output to stop speaker noise when playback is done
    pwm_set_enabled(slice_num, false);
    pwm_set_gpio_level(gpio_pin, 0); //set to 0
    gpio_set_function(gpio_pin, GPIO_FUNC_SIO); //set back to normal gpio function
}

//key to the iterative FFT
//rearrange data before processing
void bit_reverse(vector<complex<float>>& s) {
    int n = s.size(); //get total number of samples in vector
    for (int i = 1, j = 0; i < n; i++) { //iterate through the array.
        int bit = n >> 1; //start with the highest bit (half of n) to calc next reversed j
        for (; j & bit; bit >>= 1) j ^= bit; //if j is set, flip to 0 and move to next sig bit
        j ^= bit;//flip to 1 the next sig bit to get the reversed index
        if (i < j) swap(s[i], s[j]); //swap samples to correct positions
    }
}
//END OF BIT REVERSE

//iterative in-place Cooley-Tukey FFT implementation. 
//vector is taken by ref. data in memory is modified directly without new copies.
void InPlace_FFT(vector<complex<float>>& s) {
    int n = s.size();
    bit_reverse(s); //call bit reverse for the samples
    //control size of sub FFTs being merged until size n.
    for (int len = 2; len <= n; len <<= 1) {
    //calculate root of unity for current stage. 
        float ang = 2.0f * PI_F / len * -1.0f;
        complex<float> wlen(cosf(ang), sinf(ang));
        //middle loop. jump through array block by block
        for (int i = 0; i < n; i += len) {
            //twiddle factor. starts at 1. updated incrementally
            complex<float> w(1.0f);
            //butterfly inner loop.
            for (int j = 0; j < len / 2; j++) {
                //multiply by twiddle factor.
                complex<float> u = s[i + j];
                complex<float> v = s[i + j + len / 2] * w;
                //combine results of sub FFTs.
                s[i + j] = u + v;
                s[i + j + len / 2] = u - v;
                //rotation update.
                w *= wlen;
            }
        }
    }
}

//get frequency and return its musical note name
string get_note_name(float freq) {
    if (freq < 16.0f) return "--"; //nothing if frequency below human hearing range and standard musical scales
    //standard midi formula to find midi note number
    int midi = round(12.0f * log2f(freq / 440.0f) + 69.0f);
    //use mod to find where note is in the octave
    int note_idx = midi % 12;
    //determine which octave note belongs to.
    int oct = (midi / 12) - 1;
    //error handle.
    if (note_idx < 0 || note_idx > 11) return "??";
    //combine the note letter with the octave number in one string.
    return notenames[note_idx] + to_string(oct);
}

int main() {
    //initialising
    stdio_init_all();
    adc_init();
    adc_gpio_init(26); //gpio 26 for adc microphone input
    adc_select_input(0);
    const uint speaker_pin = 15; 
    //initialising finished

    //constant loop to listen to infinitely check for commands sent via USB serial
    while (true) {
        int c = getchar_timeout_us(1000);
        ////for when the record button is pressed on the website
        if (c == 'R') { 
            audio_record_pointer = 0;
            float max_magnitude = -1.0f;
            float strongest_freq = 0.0f;
            string strongest_note = "--";
            float best_spectrum[128] = {0.0f};

            //iterating through blocks of time to capture audio
            for (int block = 0; block < total_blocks; block++) {
                printf("BUSY:RECORDING\n");
                sample_index = 0;
                rdy_block = false;
                struct repeating_timer timer;
                //-16 to sample at 62.5kHz (16us per sample).
                //calls the timer callback function every 16us, sampling audio
                //buffers are filled until N samples are collected. Timer then stops and moves to FFT processing for the block.
                //repeat until total blocks are recorded.
                add_repeating_timer_us(-16, timer_callback, NULL, &timer);
                while (!rdy_block) { tight_loop_contents(); } //pause code until buffer is filled.
                cancel_repeating_timer(&timer);
                
                //performing FFT on blocks
                InPlace_FFT(samples);
                bool peak_in_this_block = false;
                //scan results to find peak freq
                //first 10 bins skipped to ignore low freq and DC offset noise
                for (int i = 10; i < N / 2; i++) {
                    float mag = abs(samples[i]);
                    if (mag > max_magnitude) {
                        max_magnitude = mag;
                        strongest_freq = (float)i * (fs / (float)N); //convert into Hz value
                        strongest_note = get_note_name(strongest_freq); //get note name
                        peak_in_this_block = true;
                    }
                }
                
                if (peak_in_this_block) {
                    for(int b = 0; b < 128; b++) {
                        float sum = 0;
                        for(int j = 0; j < 16; j++) { sum += abs(samples[b*16 + j + 10]); }
                        best_spectrum[b] = sum / 16.0f; //compress fft to 128 bins to send to visualier for website
                    }
                }
            }

            if (max_magnitude > 0.005f) {
                //send note to browser to display
                printf("DATA:%s|%.1f\n", strongest_note.c_str(), strongest_freq);
                printf("SPEC:");
                for(int b = 0; b < 128; b++) {
                    printf("%.2f%s", best_spectrum[b], (b < 127 ? "," : ""));
                }
                printf("\n");
            } else {
                printf("DATA:Silent|0.0\n"); //if signal <0.005 it will report silence
            }
            play_audio(speaker_pin, 1.0f); // play audio. default to normal speed on record
        }
        //when user adjusts frewuency slider on website, 
        //it sends a command with the new pitch multiplier for playback.
        else if (c == 'P') {
            printf("DEBUG:P received\n");
            string val_str = "";
            while(true) {
                int next_c = getchar_timeout_us(2000);
                if (next_c == '\n' || next_c == '\r' || next_c < 0) break;
                val_str += (char)next_c;
            }
            float pitch = atof(val_str.c_str()); //atof converts to float
            if (pitch <= 0.0f) pitch = 1.0f; 
            //play back the audio through the connected speaker at the modified speed
            play_audio(speaker_pin, pitch);
        }
        sleep_ms(10); //sleep to prevent overheating 
    }
}