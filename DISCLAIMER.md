# Legal Disclaimer

## No Warranty — Absolutely None

**THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED.** There is no guarantee that this software works correctly, does what
it claims to do, or is safe to use. The authors make no representations about
the accuracy, reliability, completeness, or timeliness of the software.

**Use entirely at your own risk.** The authors are not responsible for any
damage to your hardware, LED drivers, lighting installations, or any other
equipment. This includes but is not limited to:

- Incorrect configuration values being written to LED drivers
- LED drivers being bricked or rendered non-functional
- Electrical damage, overheating, or fire resulting from incorrect settings
- Data loss or corruption on NFC tags
- Any other direct, indirect, incidental, or consequential damages

**This software was written entirely by AI** and has had limited real-world
testing. It may contain bugs, incorrect assumptions, or behaviors that differ
from the original Signify tools. You should always verify any configuration
changes against the LED module's datasheet and test with appropriate safety
measures in place.

## Intended Use

This project is intended for **legitimate configuration of LED drivers that you
own**. It provides an affordable alternative to proprietary NFC programming
hardware for adjusting settings (such as output current) on SimpleSet-enabled
LED drivers.

## Reverse Engineering

This software was developed through clean-room reverse engineering of the NFC
communication protocol used by SimpleSet LED drivers, which is based on the
publicly documented ISO 15693 and ST M24LR standards. The authors believe this
constitutes lawful interoperability research under DMCA Section 1201(f)
(US) and EU Software Directive Article 6.

**This project does not contain, distribute, or circumvent any copy protection.**
The NFC password mechanism in these LED drivers is a configuration access
control, not a copy protection measure.

The RF passwords used by these LED drivers are stored as **unencrypted,
unobfuscated, plain ASCII text strings** in the freely distributed DLL.
They are visible to anyone who opens the file in a hex editor. The password
presentation mechanism itself is publicly documented in the ST M24LR datasheet
(ISO 15693 custom command 0xB3). The extraction utility in this project merely
automates locating these plain-text strings.

## Trademarks

- "Signify", "Philips", and "MultiOne" are trademarks of Signify N.V.
- "FEIG" is a trademark of FEIG Electronic GmbH.
- This project is not affiliated with, endorsed by, or sponsored by any of
  these companies.

## Third-Party Software

This project requires Signify MultiOne software only if you wish to use the
MultiOne bridge DLL. MultiOne must be obtained directly from Signify through
their official distribution channels. **This project does not include or
redistribute any Signify software.**

The password extraction utility (`extract_passwords.py`) requires access to the
original `NfcCommandsHandler.dll` from your own legitimate MultiOne
installation. It extracts configuration data needed for NFC communication but
does not modify, redistribute, or circumvent any protection in the original
software.
