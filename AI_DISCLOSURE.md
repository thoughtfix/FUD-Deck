# AI Disclosure & Responsibility Policy

## Authorship

This project was conceived, orchestrated, and released by humans.

Opus models (through Anthropic's plugin for Visual Studio Code) were liberally used as a technical tool, particularly for error-prone tasks like reading schematics against measurements on the bench, writing the read-only probe firmware that mapped every pin, tracing Zuse's faces into vectors and checking them pixel by pixel, and comparing our codebase against other open-source projects to ensure original authors receive proper credit. AI also drove the device over its serial port to take screenshots and test changes on real hardware. Even so, every architectural and design choice was human-guided, and the hardware findings (including the battery polarity warning) were checked by a human with a meter.

## Code Accountability vs. Tool Usage

We want to be entirely transparent about where responsibility lies:

**Code Quality & Bugs:** Humans take full responsibility for the code itself, including bugs, errors, and technical flaws from the first release onward. There is no such thing as "The AI wrote the bug" when it comes to software defects. If the code breaks or malfunctions technically, that responsibility rests with the author of the commit and its approvers.

**Scope of Responsibility (Monitoring):** This firmware is a radio monitor. By default it receives and logs nearby Bluetooth, Wi-Fi, 2.4 GHz and sub-GHz signals and never attacks; its only transmission by default is the hand-fired TV-B-Gone infrared toy. Even so, the information it gathers could be misused. While we are responsible for the source code we wrote and published, we assume zero responsibility for how others choose to use it. Users are solely responsible for ensuring their use of this tool complies with all applicable local, state, national, and international laws. Unlawful tracking, harassment, surveillance of people without authorization, or any malicious misuse is strictly prohibited and entirely on the end-user. See DISCLAIMERS.md for the responsibility policy governing use of this software.

## Contributions & Future PRs

We welcome future pull requests from the community! If you use AI tools to assist in writing your contributions, we simply ask that you provide a brief AI disclosure alongside your PR.
