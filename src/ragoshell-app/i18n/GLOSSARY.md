# RagoShell translation glossary

The agreed words for the terms that appear all over the UI. Translators and
reviewers (human or machine) use this table so the same thing is called the same
thing in every dialog. Part of the plan in `docs/Localization.md`.

Rules:

1. **Placeholders stay exactly as they are**: `%1`, `%2`, `%n`, `%%`. Their order
   may change; Qt substitutes by number.
2. **Keep a leading or trailing symbol** that is part of the string: `✓ ✕ ⚠ ● ■ …`
3. **`&` marks a menu mnemonic.** Keep one `&` before a letter that exists in the
   translated word, or drop it. `&&` is a literal ampersand.
4. **Plural forms:** a message with `%n` needs one translation per plural form of
   the language (Russian: 3, Arabic: 6, Japanese and Chinese: 1; Kazakh and Kyrgyz have 2 in Qt, like English).
5. **MongoDB keywords are never translated**: operators (`$match`), commands and
   helpers (`find`, `aggregate`, `explain`, `db.collection.find()`), types
   (`ObjectId`, `Int32`, `NumberLong`), URI parts (`mongodb+srv`), `_id`, JSON,
   BSON, TLS, SSH, SCRAM-SHA-256, GSSAPI, TTL, WiredTiger.
6. **Product name** `RagoShell` and `MongoDB` stay as they are: never translated, never transliterated
   (Arabic, Russian, Japanese and Chinese sentences keep the Latin "RagoShell"). The app inserts the name with
   a placeholder (`%1`, `Brand.productName`), so it is not part of the translatable text; any suffix the language
   attaches to it (`-dan`, `'yu`) depends on how "RagoShell" sounds, so it is checked by a native reviewer.
7. **Numbers:** write them with Latin digits. Do not convert digits.
8. **Tone:** neutral and short; the same register as a desktop developer tool.
   Use the polite/formal "you" only where the language needs one (no "du"/"ты"
   in sentences that address the user; prefer imperative or infinitive in
   buttons).
9. When a term below says "keep", leave the English word in Latin script, even in
   a Cyrillic, Arabic or CJK sentence.

## Terms

| English | de | fr | es | pt_BR | ru | tr |
|---|---|---|---|---|---|---|
| database | Datenbank | base de données | base de datos | banco de dados | база данных | veritabanı |
| collection | Collection (keep) | collection | colección | coleção | коллекция | koleksiyon |
| document | Dokument | document | documento | documento | документ | belge |
| field | Feld | champ | campo | campo | поле | alan |
| index | Index | index | índice | índice | индекс | indeks |
| view (MongoDB view) | View (keep) | vue | vista | view (keep) | представление | görünüm |
| replica set | Replikatset | jeu de réplicas | conjunto de réplicas | conjunto de réplicas | набор реплик | replika kümesi |
| primary / secondary | Primary / Secondary (keep) | primaire / secondaire | primario / secundario | primário / secundário | первичный / вторичный | birincil / ikincil |
| shard | Shard | shard | shard | shard | шард | shard |
| cursor | Cursor | curseur | cursor | cursor | курсор | imleç |
| pipeline | Pipeline | pipeline | pipeline | pipeline | конвейер | işlem hattı |
| stage | Stufe | étape | etapa | estágio | стадия | aşama |
| aggregation | Aggregation | agrégation | agregación | agregação | агрегация | toplama |
| query | Abfrage | requête | consulta | consulta | запрос | sorgu |
| filter | Filter | filtre | filtro | filtro | фильтр | filtre |
| projection | Projektion | projection | proyección | projeção | проекция | projeksiyon |
| sort | Sortierung | tri | orden | ordenação | сортировка | sıralama |
| connection | Verbindung | connexion | conexión | conexão | подключение | bağlantı |
| server | Server | serveur | servidor | servidor | сервер | sunucu |
| user | Benutzer | utilisateur | usuario | usuário | пользователь | kullanıcı |
| role | Rolle | rôle | rol | função | роль | rol |
| shell | Shell (keep) | Shell (keep) | Shell (keep) | Shell (keep) | Shell (keep) | Shell (keep) |
| explain (query plan) | Ausführungsplan | plan d'exécution | plan de ejecución | plano de execução | план выполнения | yürütme planı |
| schema | Schema | schéma | esquema | esquema | схема | şema |
| import / export | Import / Export | import / export | importar / exportar | importar / exportar | импорт / экспорт | içe aktar / dışa aktar |
| function (stored) | Funktion | fonction | función | função | функция | fonksiyon |
| drop (delete for good) | Löschen (endgültig) | supprimer (définitivement) | eliminar | excluir | удалить | sil (kalıcı) |
| capped collection | Capped Collection (keep) | collection plafonnée | colección limitada | coleção limitada | ограниченная коллекция | sınırlı koleksiyon |
| time series | Zeitreihen | séries temporelles | series temporales | séries temporais | временные ряды | zaman serisi |
| preferences | Einstellungen | Préférences | Preferencias | Preferências | Настройки | Tercihler |
| log | Protokoll | journal | registro | registro | журнал | günlük |

| English | az | kk | ky | uz | hi | ja | zh_CN | ar |
|---|---|---|---|---|---|---|---|---|
| database | verilənlər bazası | дерекқор | маалымат базасы | maʼlumotlar bazasi | डेटाबेस | データベース | 数据库 | قاعدة بيانات |
| collection | kolleksiya | коллекция | коллекция | kolleksiya | कलेक्शन | コレクション | 集合 | مجموعة |
| document | sənəd | құжат | документ | hujjat | डॉक्यूमेंट | ドキュメント | 文档 | مستند |
| field | sahə | өріс | талаа | maydon | फ़ील्ड | フィールド | 字段 | حقل |
| index | indeks | индекс | индекс | indeks | इंडेक्स | インデックス | 索引 | فهرس |
| view (MongoDB view) | görünüş | көрініс | көрүнүш | koʻrinish | व्यू | ビュー | 视图 | عرض |
| replica set | replika dəsti | реплика жиыны | реплика топтому | replika toʻplami | रेप्लिका सेट | レプリカセット | 副本集 | مجموعة النسخ المتماثلة |
| primary / secondary | əsas / ikinci dərəcəli | бастапқы / қосалқы | негизги / кошумча | asosiy / ikkilamchi | प्राइमरी / सेकेंडरी | プライマリ / セカンダリ | 主节点 / 从节点 | الأساسي / الثانوي |
| shard | şard | шард | шард | shard | शार्ड | シャード | 分片 | شارد |
| cursor | kursor | курсор | курсор | kursor | कर्सर | カーソル | 游标 | مؤشر |
| pipeline | pipeline | конвейер | конвейер | pipeline | पाइपलाइन | パイプライン | 管道 | خط معالجة |
| stage | mərhələ | кезең | этап | bosqich | चरण | ステージ | 阶段 | مرحلة |
| aggregation | aqreqasiya | агрегация | агрегация | agregatsiya | एग्रीगेशन | 集計 | 聚合 | تجميع |
| query | sorğu | сұрау | суроо | soʻrov | क्वेरी | クエリ | 查询 | استعلام |
| filter | filtr | сүзгі | чыпка | filtr | फ़िल्टर | フィルター | 筛选 | تصفية |
| projection | proyeksiya | проекция | проекция | proyeksiya | प्रोजेक्शन | プロジェクション | 投影 | إسقاط |
| sort | sıralama | сұрыптау | иреттөө | saralash | क्रमबद्ध करना | ソート | 排序 | ترتيب |
| connection | bağlantı | қосылым | туташуу | ulanish | कनेक्शन | 接続 | 连接 | اتصال |
| server | server | сервер | сервер | server | सर्वर | サーバー | 服务器 | خادم |
| user | istifadəçi | пайдаланушы | колдонуучу | foydalanuvchi | उपयोगकर्ता | ユーザー | 用户 | مستخدم |
| role | rol | рөл | роль | rol | भूमिका | ロール | 角色 | دور |
| shell | Shell (keep) | Shell (keep) | Shell (keep) | Shell (keep) | शेल | シェル | Shell (keep) | شل |
| explain (query plan) | icra planı | орындау жоспары | аткаруу планы | bajarilish rejasi | एक्ज़ीक्यूशन प्लान | 実行計画 | 执行计划 | خطة التنفيذ |
| schema | sxem | схема | схема | sxema | स्कीमा | スキーマ | 架构 | مخطط |
| import / export | idxal / ixrac | импорттау / экспорттау | импорттоо / экспорттоо | import / eksport | इम्पोर्ट / एक्सपोर्ट | インポート / エクスポート | 导入 / 导出 | استيراد / تصدير |
| function (stored) | funksiya | функция | функция | funksiya | फ़ंक्शन | 関数 | 函数 | دالة |
| drop (delete for good) | sil (birdəfəlik) | жою (мүлдем) | өчүрүү (биротоло) | oʻchirish (butunlay) | हटाएँ (स्थायी रूप से) | 削除 (完全に) | 删除 (永久) | حذف (نهائيًا) |
| capped collection | məhdud kolleksiya | шектеулі коллекция | чектелген коллекция | cheklangan kolleksiya | कैप्ड कलेक्शन | キャップ付きコレクション | 固定集合 | مجموعة محدودة الحجم |
| time series | zaman seriyası | уақыт қатары | убакыт катарлары | vaqt qatori | टाइम सीरीज़ | 時系列 | 时间序列 | سلسلة زمنية |
| preferences | Tənzimləmələr | Параметрлер | Жөндөөлөр | Sozlamalar | प्राथमिकताएँ | 設定 | 偏好设置 | التفضيلات |
| log | jurnal | журнал | журнал | jurnal | लॉग | ログ | 日志 | السجل |

## Language notes

- **zh_CN:** Simplified Chinese. Use full-width punctuation (，。：（）) and no space
  between Chinese and Latin text.
- **ja:** Use です・ます in sentences, the noun form in buttons and menus. Katakana
  long-vowel mark in loanwords of 3+ syllables (サーバー, ユーザー).
- **ar:** Modern Standard Arabic. Latin digits. Use the Arabic comma "،" and
  question mark "؟". Keep technical English words in Latin script.
- **hi:** Devanagari, keep widely used English technical terms transliterated as
  in the table.
- **tr / az:** Respect the dotted and dotless i (İ i, I ı).
- **kk / ky:** Cyrillic. Kazakh letters ә, ғ, қ, ң, ө, ұ, ү, һ, і; Kyrgyz ң, ө, ү.
- **uz:** Latin alphabet (2021 orthography): `oʻ`, `gʻ` with U+02BB, `sh`, `ch`.
- **az:** Latin alphabet with ə, ğ, ı, ö, ü, ç, ş.
