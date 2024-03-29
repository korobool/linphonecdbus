/*
AndroidContext.java
Copyright (C) 2014  Belledonne Communications, Grenoble, France

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
package org.linphone.mediastream;

import android.annotation.TargetApi;
import android.content.Context;
import android.media.AudioManager;
import android.os.Build;

@TargetApi(Build.VERSION_CODES.JELLY_BEAN_MR2)
public class MediastreamerAndroidContext {
	private native void setDeviceFavoriteSampleRate(int samplerate);
	private native void setDeviceFavoriteBufferSize(int bufferSize);
	
	private MediastreamerAndroidContext() {
		
	
	}
	
	public static void setContext(Object c) {
		if (c == null)
			return;
		
		Context context = (Context)c;
		int bufferSize = 64;
		int sampleRate = 44100;
		MediastreamerAndroidContext mac = new MediastreamerAndroidContext();
		// When using the OpenSLES sound card, the system is capable of giving us the best values to use for the buffer size and the sample rate
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT)
		{
	        AudioManager audiomanager = (AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
	        String bufferProperty = audiomanager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER);
	        bufferSize = parseInt(bufferProperty, bufferSize);
	        String sampleRateProperty = audiomanager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE);
	        sampleRate = parseInt(sampleRateProperty, sampleRate);
	        Log.i("Setting buffer size to " + bufferSize + " and sample rate to " + sampleRate + " for OpenSLES MS sound card.");
	        mac.setDeviceFavoriteSampleRate(sampleRate);
	        mac.setDeviceFavoriteBufferSize(bufferSize);
		} else {
			Log.i("Android < 4.4 detected, android context not used.");
		}
	}
	
	private static int parseInt(String value, int defaultValue) {
		int returnedValue = defaultValue;
		try {
			returnedValue = Integer.parseInt(value);
		} catch (NumberFormatException nfe) {
			Log.e("Can't parse " + value + " to integer ; using default value " + defaultValue);
		}
		return returnedValue;
	}
}
