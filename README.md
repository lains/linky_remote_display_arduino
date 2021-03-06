# Introduction

This repository contains an Arduino project to decode the french Linky energy meter data (aka TIC) and display power consumption in real-time on an [Adafruit I2C LCD shield for Arduino](https://www.adafruit.com/product/772).

In order to receive the TIC data from the energy, I solderde home-made a Linky serial adapter including optocoupler and transitor-based level shifter. Examples can be found here: https://faire-ca-soi-meme.fr/domotique/2016/09/12/module-teleinformation-tic/

Once the serial adapter is connected to the Arduino's GND, Vcc and Serial RX (PIN on Arduino Uno), the Arduino will be able to receive TIC stream data sent by the meter.

The software is currently configured to decode Linky data in standard TIC mode (9600 bauds), which is not the default built-in mode for Linky meters. In order to switch to this more verbose mode (that also provides more data), you need to make a request to your energy vendor.
As an alternative, the code can be slightly tweaked to switch to historical mode and to work at 1200 bauds (this is the default mode for Linky meters).

I use the LCD to display the withdrawn power in real-time.

In order to compile, you will have to import two external libraries that this code depends on:
* AdaFruit RGB LCD shield library that can be installed directly from the built-in Arduino IDE's library manager
* LibTeleinfo from [Charles Hallard](https://github.com/hallard/LibTeleinfo) (use Arduino IDE's import library as zip)
