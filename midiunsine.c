/*
    Copyright (C) 2004 Ian Esten

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#include <jack/jack.h>
#include <jack/midiport.h>

typedef float complex cplx;

jack_client_t *client;
jack_port_t *input_port;
jack_port_t *output_port;
jack_default_audio_sample_t ramp=0.0;
jack_default_audio_sample_t note_on;
int note = -1;
jack_default_audio_sample_t * note_frqs = NULL;
cplx * fftbuf = NULL;
cplx * scratch = NULL;
jack_nframes_t bufsize = 128;
jack_nframes_t samplerate = 48000;

static void signal_handler(int sig)
{
	jack_client_close(client);
	fprintf(stderr, "signal received, exiting ...\n");
	exit(0);
}

/* https://rosettacode.org/wiki/Fast_Fourier_transform#C */

void _fft(cplx buf[], cplx out[], int n, int step)
{
	if (step < n) {
		_fft(out, buf, n, step * 2);
		_fft(out + step, buf + step, n, step * 2);

		for (int i = 0; i < n; i += 2 * step) {
			cplx t = cexp(-I * M_PI * i / n) * out[i + step];
			buf[i / 2]     = out[i] + t;
			buf[(i + n)/2] = out[i] - t;
		}
	}
}

void fft(cplx * buf, cplx * scratch, int n)
{
	for (int i = 0; i < n; i++) scratch[i] = buf[i];
	_fft(buf, scratch, n, 1);
}

/*
void show(const char * s, cplx buf[]) {
	printf("%s", s);
	for (int i = 0; i < 8; i++)
		if (!cimag(buf[i]))
			printf("%g ", creal(buf[i]));
		else
			printf("(%g, %g) ", creal(buf[i]), cimag(buf[i]));
}

int main()
{
	cplx buf[] = {1, 1, 1, 1, 0, 0, 0, 0};

	show("Data: ", buf);
	fft(buf, 8);
	show("\nFFT : ", buf);

	return 0;
}
*/

void calc_note_frqs()
{
	int i;
	if (note_frqs) free(note_frqs);
	note_frqs = (jack_default_audio_sample_t *)malloc(sizeof(jack_default_audio_sample_t) * bufsize);
	if (scratch) free(scratch);
	scratch = (cplx *)malloc(sizeof(cplx) * bufsize);
	if (fftbuf) free(fftbuf);
	fftbuf = (cplx *)malloc(sizeof(cplx) * bufsize);
	for(i=0; i<bufsize; i++) {
		note_frqs[i] = log2((i * samplerate / bufsize) / (2.0 * 440.0 / 32.0)) * 12.0 + 9.0;
		// note_frqs[i] = (2.0 * 440.0 / 32.0) * pow(2, (((jack_default_audio_sample_t)i - 9.0) / 12.0)) / srate;
	}
}

int process(jack_nframes_t nframes, void *arg)
{
	if (nframes != bufsize) {
		printf("process got wrong buffer size (%lld vs %lld)\n", (long long)nframes, (long long)bufsize);
		exit(1);
		return 0;
	}
	int i, j;
	jack_default_audio_sample_t *in = (jack_default_audio_sample_t *) jack_port_get_buffer(input_port, nframes);
	for (i = 0; i < bufsize; ++i) fftbuf[i] = in[i];
	fft(fftbuf, scratch, bufsize);
	j = -1;
	for (i = 0; i < bufsize / 2; ++i) {
		if (cabs(fftbuf[i]) / bufsize > 1.0/128.0 && (j == -1 || cabs(fftbuf[i]) > cabs(fftbuf[j])))
			j = i;
	}
	int n = j == -1 ? -1 : ((int)round(note_frqs[j]) - 24);
	void* port_buf = jack_port_get_buffer(output_port, nframes);
	unsigned char* buffer;
	jack_midi_clear_buffer(port_buf);
	if (n == note) return 0;
	if (note != -1) {
		buffer = jack_midi_event_reserve(port_buf, 0, 3);
		printf("midiunsine: OFF %d (-> %d)\n", (int)note, (int)n);
		buffer[2] = 64;		/* velocity */
		buffer[1] = note;
		buffer[0] = 0x80;	/* note off */
	}
	if(n != -1) {
		buffer = jack_midi_event_reserve(port_buf, 0, 3);
		printf("midiunsine: ON  (%d ->) %d\n", (int)note, (int)n);
		buffer[2] = 64;		/* velocity */
		buffer[1] = n;
		buffer[0] = 0x90;	/* note on */
	}
	note = n;
	return 0;
}

int buffer_size_callback(jack_nframes_t bs, void *arg)
{
	printf("the buffer size is now %" PRIu32 "/sec\n", bs);
	bufsize = bs;
	calc_note_frqs();
	return 0;
}

int srate(jack_nframes_t nframes, void *arg)
{
	printf("the sample rate is now %" PRIu32 "/sec\n", nframes);
	samplerate = nframes;
	calc_note_frqs();
	return 0;
}

void jack_shutdown(void *arg)
{
	exit(1);
}

int main(int narg, char **args)
{
	jack_client_t *client;

	if ((client = jack_client_open ("midiunsine", JackNullOption, NULL)) == 0)
	{
		fprintf(stderr, "jack server not running?\n");
		return 1;
	}

	bufsize = jack_get_buffer_size(client);
	samplerate = jack_get_sample_rate(client);
	calc_note_frqs();

	jack_set_process_callback (client, process, 0);

	jack_set_buffer_size_callback (client, buffer_size_callback, 0);
	jack_set_sample_rate_callback (client, srate, 0);

	jack_on_shutdown (client, jack_shutdown, 0);

	input_port = jack_port_register (client, "audio_in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	output_port = jack_port_register (client, "mini_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

	if (jack_activate (client))
	{
		fprintf(stderr, "cannot activate client");
		return 1;
	}

	/* install a signal handler to properly quits jack client */
#ifndef WIN32
	signal(SIGQUIT, signal_handler);
	signal(SIGHUP, signal_handler);
#endif
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	/* run until interrupted */
#ifdef WIN32
	Sleep(-1);
#else
	sleep(-1);
#endif

	jack_client_close(client);
	exit (0);
}

