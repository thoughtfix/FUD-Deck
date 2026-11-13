# Experimental Hardware & Responsible Use Disclaimer

FUD-Deck is counter-surveillance firmware. It listens for the Bluetooth, Wi-Fi,
2.4 GHz and sub-GHz signals of nearby devices and networks and reports what it
sees. By default it is receive-only: no attack modes are enabled, its scans are
passive, and it does not join networks, deauthenticate, spoof, or attack
anything. The one thing it transmits by default is the TV-B-Gone toy, an
infrared signal fired by hand. Any scanning, logging, or monitoring made
available by this software, or by devices running it, is intended for lawful
personal awareness and educational purposes only.

Use these features only where it is legal to do so, and only in ways that
respect other people. Radio monitoring and the capture or logging of signals may
be regulated in your jurisdiction. Do not use this software to stalk, harass,
track, profile, or otherwise unlawfully monitor any person, and do not use it to
access, disrupt, damage, or interfere with systems or equipment. Future versions
may offer modes that transmit; they will be off by default, and turning one on
can reveal your device, and with it your privacy or location, to others nearby.

Experimental / Beta Disclaimer: The software, firmware, examples,
configurations, and demonstrations provided with this project may be beta,
experimental, incomplete, or subject to change. They are provided "as is" and
without guarantees or warranties of any kind, express or implied, including any
guarantee of fitness for a particular purpose, reliability, compatibility,
performance, or continued availability. Detection is best-effort: it may miss
real devices and it may misclassify harmless ones, and examples may not behave
the same on every unit or configuration.

Hardware Modification Disclaimer: Flashing this firmware or otherwise modifying
your device is performed entirely at your own risk. Flashing replaces the
firmware your board shipped with. Incorrect flashing, wiring, power connections,
or configuration may damage or permanently disable your hardware, including the
ESP32-DIV V2, its ESP32-S3, its radio modules, or any connected equipment such
as a battery or SD card. The ESP32-DIV V2 has no protection against a battery
connected backwards, and a "+" mark beside its battery jack is misleading: read
[docs/ESP32-DIV_V2_Power](docs/ESP32-DIV_V2_Power/ESP32-DIV_V2_Power.md) before
connecting one. Back up the stock firmware before writing anything. If you
damage, destroy, or otherwise render your hardware unusable while flashing or
modifying it, you are solely responsible for the resulting damage, costs,
repairs, or replacement. There are many communities passionate about ESP32
projects, and anyone unsure of their work is encouraged to read and ask
questions before flashing or plugging things in.

You are solely responsible for ensuring that your use of this software, device,
and any associated modifications complies with all applicable local, state,
national, and international laws, regulations, policies, and contractual
obligations. This is not a small deal. This is a big deal. In the United States,
a good place to start (not stop) is FCC Title 47.
https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15

The developers and distributors of this project do not authorize or condone
unauthorized access, misuse, or unlawful activity, and are not responsible for
any damage, loss, disruption, hardware failure, data loss, or legal consequences
resulting from the use or misuse of the project.

Know and obey all applicable laws. Monitor only where and how you are permitted
to. Flash and modify hardware only if you understand the risks and accept
responsibility for the results.
