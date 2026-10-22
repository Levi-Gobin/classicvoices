package com.levigobin.classicvoices;

import java.util.Locale;

/** A short sentence in each language the voices speak. */
final class Samples {
    private Samples() {}

    static String forLanguage(Locale l) {
        switch (l.getLanguage()) {
            case "ar": return "مرحبا، هذا صوتي.";
            case "cs": return "Dobrý den, toto je můj hlas.";
            case "da": return "Hej, dette er min stemme.";
            case "de": return "Hallo, das ist meine Stimme.";
            case "el": return "Γεια σας, αυτή είναι η φωνή μου.";
            case "es": return "Hola, esta es mi voz.";
            case "fi": return "Hei, tämä on minun ääneni.";
            case "fr": return "Bonjour, voici ma voix.";
            case "he": return "שלום, זה הקול שלי.";
            case "hi": return "नमस्ते, यह मेरी आवाज़ है।";
            case "hu": return "Helló, ez az én hangom.";
            case "id": return "Halo, ini suara saya.";
            case "it": return "Ciao, questa è la mia voce.";
            case "ja": return "こんにちは、これは私の声です。";
            case "ko": return "안녕하세요, 제 목소리입니다.";
            case "nb": case "no": return "Hei, dette er stemmen min.";
            case "nl": return "Hallo, dit is mijn stem.";
            case "pl": return "Cześć, to jest mój głos.";
            case "pt": return "Olá, esta é a minha voz.";
            case "ro": return "Bună, aceasta este vocea mea.";
            case "ru": return "Здравствуйте, это мой голос.";
            case "sk": return "Dobrý deň, toto je môj hlas.";
            case "sv": return "Hej, det här är min röst.";
            case "th": return "สวัสดี นี่คือเสียงของฉัน";
            case "tr": return "Merhaba, bu benim sesim.";
            case "zh": return "你好，这是我的声音。";
            default: return "Hello, this is my voice.";
        }
    }
}
