# Company Synopsis & System Overview
The Walt Disney Company operates some of the world’s most sophisticated ride systems, where passenger safety depends on rapid, deterministic responses to sensors and operator commands. Ride vehicles and track segments rely on distributed safety nodes that continuously monitor load, brake conditions, and emergency stop inputs. These systems must meet strict timing deadlines to avoid catastrophic failure. Real-time performance is essential: missing deadlines could delay braking, misreport sensor data, or fail to detect an overload condition. This prototype models one such safety node.

This project implements a proof-of-concept ride safety node similar to the distributed microcontrollers used in modern roller coaster control systems. The ESP32 monitors a simulated track-load sensor, operator E-Stop input, and a manual reset button. It drives dedicated indicator LEDs for system status, overload conditions, and emergency braking, while also producing periodic telemetry messages for a higher-level supervisory controller. The purpose of this design is to demonstrate clear partitioning between Hard, Firm, and Soft real time workloads while showing how critical safety functions can preempt and override lower-priority system activities. The node models realistic safety behavior: overload warnings, continuous alarm states, E-Stop latching, and degraded-mode telemetry once an emergency occurs.

# Task Table

![alt text](<Screenshot 2025-12-05 191401.png>)


# Engineering Analysis 
## Scheduler Fit: 
How do your task priorities / RTOS settings guarantee every H task’s deadline in Wokwi? Cite one timestamp pair that proves it.

## Race‑Proofing: 
Where could a race occur? Show the exact line(s) you protected and which primitive solved it.

## Worst‑Case Spike: 
Describe the heaviest load you threw at the prototype (e.g., sensor spam, comm burst). What margin (of time) remained before an H deadline would slip?

## Design Trade‑off: 
Name one feature you didn’t add (or simplified) to keep timing predictable. Why was that the right call for your chosen company?


### AI Use

#### Troubleshooting Reset ISR & Task

I said:
assert failed: 0x4008b966 <cached disabled>:1358 (<cached disabled>)
Backtrace: 0x400818ae:0x3ffb0de0 0x40085991:0x3ffb0e00 0x4008ba09:0x3ffb0e20 0x40085f2a:0x3ffb0f40 0x40082a54:0x3ffb0f60 0x4008303a:0x3ffb0f90 0x400830ca:0x3ffb0fc0 0x40082de9:0x3ffb0ff0 0x400845e3:0x3ffb4540 0x400d2113:0x3ffb4560 0x400870d2:0x3ffb4580 0x40086321:0x3ffb45a0

ELF file SHA256: 6e83b5b77

Rebooting...
ets Jul 29 2019 12:21:46

rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:1156
load:0x40078000,len:11456
ho 0 tail 12 room 4
load:0x40080400,len:2972
entry 0x400805dc
[       0 ms] [Telemetry] adc=   0, alerts_in_queue= 0, emergency=0
[       0 ms] [BOOT] Ride Safety Node started. Threshold=3000
[     200 ms] [Telemetry] adc=   0, alerts_in_queue= 0, emergency=0
[     400 ms] [Telemetry] adc

ChatGPT said:
Okay — you're seeing THIS:

assert failed: 0x4008b966 <cached disabled>:1358 (<cached disabled>)
Backtrace...
Rebooting...
E (xxx) gpio: gpio_isr_handler_add(): GPIO isr service is not installed


❗ Your RESET ISR is being registered BEFORE gpio_install_isr_service() is called.

This is the only thing that produces that specific assert + reboot pattern.

Even if the E-STOP ISR is correct,
if the RESET ISR is added too early → boom → assert → reboot loop.

✅ Fix it by ensuring THIS exact order inside app_main()
Correct order:
// ------------- ISR service -------------
gpio_install_isr_service(0);   // MUST RUN BEFORE ANY ISR HANDLERS

// Add E-STOP ISR
gpio_isr_handler_add(ESTOP_PIN, estop_isr_handler, NULL);

// Add RESET ISR
gpio_isr_handler_add(RESET_PIN, reset_isr_handler, NULL);


