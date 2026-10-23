# PlayTime

A PS5 ELF payload that serves a web page listing every installed game, shows how long each one has been played (per console user), and lets you edit the play time.

Open it on the console's own browser or from a phone / laptop on the same network.

![Playime](https://raw.githubusercontent.com/ghost1372/App-Resources/refs/heads/main/PS5-Payloads/PlayTime/1.png)
![Playime](https://raw.githubusercontent.com/ghost1372/App-Resources/refs/heads/main/PS5-Payloads/PlayTime/2.png)
![Playime](https://raw.githubusercontent.com/ghost1372/App-Resources/refs/heads/main/PS5-Payloads/PlayTime/3.png)
![Playime](https://raw.githubusercontent.com/ghost1372/App-Resources/refs/heads/main/PS5-Payloads/PlayTime/4.jpg)


## Requirements

- A PS5 payload loader that can run ELF payloads. PlayTime works on all firmware versions your loader supports.
- The console and your phone / laptop must be on the same network if you want to open the page from another device.

> [!NOTE]
> Only installed games are listed (title IDs starting with `CUSA` or `PPSA`).

## How to Use

1. Download the latest `PlayTime-<version>.elf` from the [GitHub Releases](https://github.com/ghost1372/PlayTime/releases) page.
2. Send the ELF to your PS5 using [Payload Sender](https://github.com/ghost1372/payloadsender) or [payload manager](https://github.com/itsPLK/ps5-payload-manager).
3. The PS5 browser opens `http://127.0.0.1:7788` automatically.
4. From a phone or laptop on the same network, open `http://<ps5-ip>:7788`. The address is also shown at the top of the page.

## Features

- Total play time, games played / not played, average per game, play time by user and by platform.
- Most played and recently played lists.
- Search by name or title ID, filter PS5 / PS4, sort by most played, recent, A-Z or title ID.
- Dark and light theme.
- Edit play time per user (see below).

## Editing play time

Open a game, press **Edit play time** under a user, adjust hours and minutes with the `-` / `+` buttons (and the quick +/-5/10/15 buttons) and press **Save changes**.

- Users are shown by their console account name; `User N` is used if the name cannot be read.
- Before the first write of a run, `app.db` is copied to `/data/playtime_app.db.bak`. If the copy fails, nothing is written.
- The game must already have a row for that user.
- The system may keep its own copy of the numbers in memory, so a reboot can be needed before the PS5 UI shows the change, and it may overwrite the value the next time the game runs.

## How to compile

> [!NOTE]
> You need Linux, but you can use Windows and WSL2 to compile this project.

1. Install or prepare the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk).
2. From the project directory, run:

   ```sh
   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
   make
   ```

The output is `PlayTime.elf`.

