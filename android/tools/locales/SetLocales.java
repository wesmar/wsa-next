// WSA-Next © 2026 Marek Wesołowski - wesmar
//
// Sets Android's system language list the way Settings > System > Languages does
// (LocalePicker.updateLocales), for collect-play-apps.ps1: Google Play delivers an app's
// language splits for every language on the device's list, so a reference installation that
// lists them all collects the apps with every language. The first language stays the UI's.
//
// Reflection only, so it builds without android.jar:
//   javac --release 8 -d classes SetLocales.java
//   d8 --min-api 30 --output . classes/SetLocales.class          (d8: Google's SDK build-tools)
//   adb push classes.dex /data/local/tmp/locales.dex
//   adb shell CLASSPATH=/data/local/tmp/locales.dex app_process /system/bin SetLocales "pl-PL,en-US,de,..."
// It needs root (adb root) and prints the list Android now uses.
public class SetLocales {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            System.err.println("usage: SetLocales <comma-separated BCP 47 language tags>");
            System.exit(2);
        }
        Class<?> configurationClass = Class.forName("android.content.res.Configuration");
        Class<?> localeListClass = Class.forName("android.os.LocaleList");
        Object service = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        Object config = service.getClass().getMethod("getConfiguration").invoke(service);
        Object list = localeListClass.getMethod("forLanguageTags", String.class).invoke(null, args[0]);
        configurationClass.getMethod("setLocales", localeListClass).invoke(config, list);
        configurationClass.getField("userSetLocale").setBoolean(config, true);
        service.getClass().getMethod("updatePersistentConfigurationWithAttribution",
                configurationClass, String.class, String.class).invoke(service, config, "com.android.shell", null);
        Object now = service.getClass().getMethod("getConfiguration").invoke(service);
        System.out.println(configurationClass.getMethod("getLocales").invoke(now));
    }
}
