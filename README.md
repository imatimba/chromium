# darker-chromium

A project to apply a dark theme to Chromium internal pages and fix some bugs related to white flashes.

## Features

* Fixes white flashes on navigation (current or new tab) and when opening link on new window.  
* Themes internal pages using Firefox’s Developer Edition Dark theme colors, WIP

## Installation

**Windows:**

You can download the latest installer from [Releases](https://github.com/imatimba/darker-chromium/releases).  
If you want to compile it yourself there are 2 types of patch files provided, full and individual.  
You will need to do a full Chromium checkout or use the tarballs provided in [this](https://github.com/zcbenz/chromium-source-tarball) repo as the offical source tarballs only contain dependencies for Linux.

**Linux:**

I can't provide installers at the moment, I need to figure out how to correctly create .deb and .rpm packages.  
You can download any of the patch files in [Releases](https://github.com/imatimba/darker-chromium/releases) and compile it yourself downloading Chromium source tarball taking as reference your Distro's Chromium compiling process (What args.gn to use, extra necessary patches, etc).

**Mac:**

I won't provide support for now as I don't know anything about Mac.

## Recommended extensions/theme

[Developer Edition Dark theme](https://chrome.google.com/webstore/detail/developer-edition-dark/lglfmldlfmbbehalkgiglehhjblbfcjo) to have the same colors on bookmarks bar and tabs section.

[Stylish](https://chrome.google.com/webstore/detail/stylish-custom-themes-for/fjnbnpbmkenffdnngjfgmeleoegfcffe) to use custom dark themes on your most visited websites.

[Dark Reader](https://chrome.google.com/webstore/detail/dark-reader/eimadpbcbfnmbkopoojfekhnkhdbieeh) to invert the colors on websites that you don't want or can't install Stylish themes.

## FAQ

**Why Flash player doesn't work?**

Chromium doesn't include Flash player, you have to install it manually.

*Windows instructions:*

You need to install it from Adobe https://get.adobe.com/flashplayer/otherversions/.  
Selecting your Windows and version “FP 24 for Opera and Chromium – PPAPI”  
After installing it I recommend to select to notify you when a new version is available, no auto update.

Then you need to change the shortcut of Chromium to enable Flash like this:

    "C:\Users\YOURUSER\AppData\Local\Chromium\Application\chrome.exe" --ppapi-flash-path="C:\Windows\System32\Macromed\Flash\pepflashplayer64_24_0_0_186.dll" --ppapi-flash-version="24.0.0.186" --allow-outdated-plugins  
Replacing YOURUSER obviously. And changing the file name/version when you update it.

If you enable “Let Chromium run in the background” you’ll have to edit the autostart registry entry too, adding the same arguments for Flash.  
The entry is located at:

    HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\Run
And the entry looks like:

    GoogleChromeAutoLaunch_DGFGHRE23463421GEDFGWQRQW  
So edit the Data adding the arguments at the end.  
You will need to edit the default shortcut and registry key every time you update Chromium.

**Why can't I sign in to my Google account and sync/use Google services?**

Chromium doesn't come with the necessary API keys to work with Google services by default, you have to provide them on compilation or runtime.  
I can't provide mine because of quota limits.

*Windows/Linux instructions:*

You need to enable api keys on your google account. For that you need to follow these steps:  
https://www.chromium.org/developers/how-tos/api-keys

A few menus have different names but it’s easy to figure that out.  
Don’t enable the optional ones.  
Google Cloud DNS is the only one that asked to enable billing for me. I didn’t enable it and all works
fine.

After that you can create the environment variables as shown in the “Providing Keys at Runtime” section.

* GOOGLE_API_KEY

  your_api_key
* GOOGLE_DEFAULT_CLIENT_ID

  your_client_id
* GOOGLE_DEFAULT_CLIENT_SECRET
  
  your_client_secret

Check https://kb.wisc.edu/cae/page.php?id=24500 if you don’t how to create environment variables for Windows  

Check your Distro's documentation for Linux.  
Usually requires adding:

    export GOOGLE_API_KEY=your_api_key
    export GOOGLE_DEFAULT_CLIENT_ID=your_client_id
    export GOOGLE_DEFAULT_CLIENT_SECRET=your_client_secret
To .bashrc or .profile

## Known Problems

* Changing WebKit default background to fix white flashes on new windows also changes the background of websites that don't specify a background color.
This affects extensions as well.  
  Sometimes making them unreadable.  
  On websites you can work around it using Stylish or Dark Reader to change the theme and make it readable again.  
  On extensions you'll need to manually edit css/js/html files of the extension.

## Preview images

![newtab](https://cloud.githubusercontent.com/assets/7434335/21759144/cbd90846-d620-11e6-8557-37e4c2078e10.png)

![settings](https://cloud.githubusercontent.com/assets/7434335/21759153/d827f238-d620-11e6-96d3-baf65e068fff.png)

![extensions](https://cloud.githubusercontent.com/assets/7434335/21759156/df58a1e2-d620-11e6-91eb-9496f38bf3d5.png)

![help](https://cloud.githubusercontent.com/assets/7434335/21759159/e39cf67c-d620-11e6-84e7-97fd067b4a4a.png)

![downloads](https://cloud.githubusercontent.com/assets/7434335/22136171/a2256244-deb1-11e6-80b1-03e1b79ab130.png)

![history](https://cloud.githubusercontent.com/assets/7434335/22136181/b1b15e84-deb1-11e6-806c-e4cd9fb53512.png)

![bookmarks](https://cloud.githubusercontent.com/assets/7434335/22136192/c3ee4080-deb1-11e6-9179-64d1423fc870.png)

![flags](https://cloud.githubusercontent.com/assets/7434335/22136196/c8eac306-deb1-11e6-8531-f02f9c6d96d6.png)

## Credits

Thanks to [hbtlabs](https://github.com/hbtlabs/chromium-white-flash-fix) for sharing the relevant lines to change to fix the white flashes.

Thanks to [KeenRivals](https://github.com/KeenRivals) and his [theme](https://github.com/KeenRivals/chrome-developer-edition-dark) that gave me the idea to use these colors.

The Chromium Authors
