// Deterministic, self-contained seed for the `samples` database used by ShellCorpusTest
// (docs/ShellCorpusRecorder.md). Compatible with MongoDB 4.0+ (`mongo`, `mongosh`, and RagoShell's
// embedded QJSEngine runtime: plain ES5, no external downloads, no server features newer than 4.0).
(function () {
    "use strict";

    var sdb = db.getSiblingDB("samples");
    sdb.dropDatabase();

    // Extract a fresh 9-byte (18-hex) timestamp+random prefix from this run's ObjectId generator
    // and combine it with a strictly monotonic 3-byte counter (000001..00630f). Every seed run
    // produces brand-new dynamic ObjectIds (exercising _id normalization) while guaranteeing
    // sort({_id: 1}) order without 24-bit counter wraparound.
    var rawFirstOid = new ObjectId().toString();
    var matchHex = rawFirstOid.match(/[0-9a-fA-F]{24}/);
    if (!matchHex) {
        throw new Error("Failed to extract 24-hex ObjectId prefix from: " + rawFirstOid);
    }
    var oidPrefix = matchHex[0].toLowerCase().substring(0, 18);
    var oidSeq = 0;

    function nextOid() {
        oidSeq += 1;
        var suffix = oidSeq.toString(16);
        while (suffix.length < 6) {
            suffix = "0" + suffix;
        }
        return new ObjectId(oidPrefix + suffix);
    }

    function padDigits(n, width) {
        var s = String(n);
        while (s.length < width) {
            s = "0" + s;
        }
        return s;
    }

    function insertBatched(coll, docs) {
        var batchSize = 1000;
        for (var i = 0; i < docs.length; i += batchSize) {
            coll.insertMany(docs.slice(i, i + batchSize), { ordered: true });
        }
    }

    function dt(iso) {
        return new Date(iso);
    }

    // ---- 1. samples.restaurants (25,359 documents, default _id_ index) --------------------------

    var restaurants = [];

    // Canonical first 7 documents overall in insertion / _id order.
    var initialSeven = [
        {
            _id: nextOid(),
            address: {
                building: "203",
                coord: [-73.97822040000001, 40.6435254],
                street: "Church Avenue",
                zipcode: "11218"
            },
            borough: "Brooklyn",
            cuisine: "Ice Cream, Gelato, Yogurt, Ices",
            grades: [
                { date: dt("2014-02-10T00:00:00Z"), grade: "A", score: 2 },
                { date: dt("2013-01-02T00:00:00Z"), grade: "A", score: 13 },
                { date: dt("2012-01-09T00:00:00Z"), grade: "A", score: 3 },
                { date: dt("2011-11-07T00:00:00Z"), grade: "P", score: 12 },
                { date: dt("2011-07-21T00:00:00Z"), grade: "A", score: 13 }
            ],
            name: "Carvel Ice Cream",
            restaurant_id: "40360076"
        },
        {
            _id: nextOid(),
            address: {
                building: "7114",
                coord: [-73.9068506, 40.6199034],
                street: "Avenue U",
                zipcode: "11234"
            },
            borough: "Brooklyn",
            cuisine: "Delicatessen",
            grades: [
                { date: dt("2014-05-29T00:00:00Z"), grade: "A", score: 10 },
                { date: dt("2014-01-14T00:00:00Z"), grade: "A", score: 10 },
                { date: dt("2013-08-03T00:00:00Z"), grade: "A", score: 8 },
                { date: dt("2012-07-18T00:00:00Z"), grade: "A", score: 10 },
                { date: dt("2012-03-09T00:00:00Z"), grade: "A", score: 13 },
                { date: dt("2011-10-14T00:00:00Z"), grade: "A", score: 9 }
            ],
            name: "Wilken'S Fine Food",
            restaurant_id: "40356483"
        },
        {
            _id: nextOid(),
            address: {
                building: "265-15",
                coord: [-73.7032601, 40.7386417],
                street: "Hillside Avenue",
                zipcode: "11004"
            },
            borough: "Queens",
            cuisine: "Ice Cream, Gelato, Yogurt, Ices",
            grades: [
                { date: dt("2014-10-28T00:00:00Z"), grade: "A", score: 9 },
                { date: dt("2013-09-18T00:00:00Z"), grade: "A", score: 10 },
                { date: dt("2012-09-20T00:00:00Z"), grade: "A", score: 13 }
            ],
            name: "Carvel Ice Cream",
            restaurant_id: "40361322"
        },
        {
            _id: nextOid(),
            address: {
                building: "6909",
                coord: [-74.0259567, 40.6353674],
                street: "3 Avenue",
                zipcode: "11209"
            },
            borough: "Brooklyn",
            cuisine: "Delicatessen",
            grades: [
                { date: dt("2014-08-21T00:00:00Z"), grade: "A", score: 4 },
                { date: dt("2014-03-05T00:00:00Z"), grade: "A", score: 3 },
                { date: dt("2013-01-10T00:00:00Z"), grade: "A", score: 10 }
            ],
            name: "Nordic Delicacies",
            restaurant_id: "40361390"
        },
        {
            _id: nextOid(),
            address: {
                building: "522",
                coord: [-73.95171, 40.767461],
                street: "East   74 Street",
                zipcode: "10021"
            },
            borough: "Manhattan",
            cuisine: "American",
            grades: [
                { date: dt("2014-09-02T00:00:00Z"), grade: "A", score: 12 },
                { date: dt("2013-12-19T00:00:00Z"), grade: "B", score: 16 },
                { date: dt("2013-05-28T00:00:00Z"), grade: "A", score: 9 },
                { date: dt("2012-12-07T00:00:00Z"), grade: "A", score: 13 },
                { date: dt("2012-03-29T00:00:00Z"), grade: "A", score: 11 }
            ],
            name: "Glorious Food",
            restaurant_id: "40361521"
        },
        {
            _id: nextOid(),
            address: {
                building: "129-08",
                coord: [-73.839297, 40.78147],
                street: "20 Avenue",
                zipcode: "11356"
            },
            borough: "Queens",
            cuisine: "Delicatessen",
            grades: [
                { date: dt("2014-08-16T00:00:00Z"), grade: "A", score: 12 },
                { date: dt("2013-08-27T00:00:00Z"), grade: "A", score: 9 },
                { date: dt("2012-09-20T00:00:00Z"), grade: "A", score: 7 },
                { date: dt("2011-09-29T00:00:00Z"), grade: "A", score: 10 }
            ],
            name: "Sal'S Deli",
            restaurant_id: "40361618"
        },
        {
            _id: nextOid(),
            address: {
                building: "284",
                coord: [-73.9829239, 40.6580753],
                street: "Prospect Park West",
                zipcode: "11215"
            },
            borough: "Brooklyn",
            cuisine: "American",
            grades: [
                { date: dt("2014-11-19T00:00:00Z"), grade: "A", score: 11 },
                { date: dt("2013-11-14T00:00:00Z"), grade: "A", score: 2 },
                { date: dt("2012-12-05T00:00:00Z"), grade: "A", score: 13 },
                { date: dt("2012-05-17T00:00:00Z"), grade: "A", score: 11 }
            ],
            name: "The Movable Feast",
            restaurant_id: "40361606"
        }
    ];

    // Canonical first 3 "Italian" documents in insertion / _id order.
    var initialItalian = [
        {
            _id: nextOid(),
            address: {
                building: "251",
                coord: [-73.9775552, 40.7432016],
                street: "East   31 Street",
                zipcode: "10016"
            },
            borough: "Manhattan",
            cuisine: "Italian",
            grades: [
                { date: dt("2014-04-22T00:00:00Z"), grade: "A", score: 13 },
                { date: dt("2013-06-19T00:00:00Z"), grade: "C", score: 32 },
                { date: dt("2012-05-22T00:00:00Z"), grade: "A", score: 12 }
            ],
            name: "Marchis Restaurant",
            restaurant_id: "40364668"
        },
        {
            _id: nextOid(),
            address: {
                building: "67",
                coord: [-74.0707363, 40.59321569999999],
                street: "Olympia Boulevard",
                zipcode: "10305"
            },
            borough: "Staten Island",
            cuisine: "Italian",
            grades: [
                { date: dt("2014-04-24T00:00:00Z"), grade: "A", score: 13 },
                { date: dt("2013-04-04T00:00:00Z"), grade: "A", score: 2 },
                { date: dt("2012-02-02T00:00:00Z"), grade: "A", score: 5 },
                { date: dt("2011-07-23T00:00:00Z"), grade: "A", score: 11 }
            ],
            name: "Crystal Room",
            restaurant_id: "40365013"
        },
        {
            _id: nextOid(),
            address: {
                building: "93",
                coord: [-73.99950489999999, 40.7169224],
                street: "Baxter Street",
                zipcode: "10013"
            },
            borough: "Manhattan",
            cuisine: "Italian",
            grades: [
                { date: dt("2014-12-15T00:00:00Z"), grade: "A", score: 9 },
                { date: dt("2013-12-06T00:00:00Z"), grade: "A", score: 10 },
                { date: dt("2012-10-23T00:00:00Z"), grade: "A", score: 12 },
                { date: dt("2012-06-04T00:00:00Z"), grade: "A", score: 11 },
                { date: dt("2012-01-12T00:00:00Z"), grade: "A", score: 13 }
            ],
            name: "Forlinis Restaurant",
            restaurant_id: "40365098"
        }
    ];

    var topAlphaItalianNames = [
        "(Lewis Drug Store) Locanda Vini E Olii",
        "101 Restaurant And Bar",
        "44 Sw Ristorante & Bar",
        "900 Park",
        "A Voce"
    ];

    var boroughs = ["Brooklyn", "Manhattan", "Queens", "Bronx", "Staten Island"];
    var defaultGradeDate = dt("2014-06-15T00:00:00Z");
    var i;

    for (i = 0; i < initialSeven.length; i += 1) {
        restaurants.push(initialSeven[i]);
    }
    for (i = 0; i < initialItalian.length; i += 1) {
        restaurants.push(initialItalian[i]);
    }
    for (i = 0; i < topAlphaItalianNames.length; i += 1) {
        restaurants.push({
            _id: nextOid(),
            address: {
                building: String(100 + i),
                coord: [-73.985, 40.748],
                street: " Broadway",
                zipcode: "10001"
            },
            borough: boroughs[i % boroughs.length],
            cuisine: "Italian",
            grades: [{ date: defaultGradeDate, grade: "A", score: 10 }],
            name: topAlphaItalianNames[i],
            restaurant_id: String(40400000 + i)
        });
    }

    // Remaining 25,344 documents across 19 cuisines (exact total = 25,359).
    // Top 3 cuisines overall: American (6183), Chinese (2418), Café/Coffee/Tea (1214); Italian = 1069.
    var cuisineFillers = [
        ["American", 6181, "Classic Diner "],
        ["Chinese", 2418, "Golden Dragon "],
        ["Café/Coffee/Tea", 1214, "Corner Cafe "],
        ["Italian", 1061, "Trattoria Italiana "],
        ["Pizza", 1163, "Slice House "],
        ["Mexican", 1120, "Taqueria Sol "],
        ["Japanese", 1100, "Sakura Kitchen "],
        ["Latin (Cuban, Dominican, Puerto Rican, South & Central American)", 1080, "Caribe Grill "],
        ["Bakery", 1060, "City Bakery "],
        ["Caribbean", 1050, "Island Spice "],
        ["Spanish", 1040, "Tapas Madrid "],
        ["Hamburgers", 1020, "Burger Counter "],
        ["Sandwiches", 1000, "Midtown Sub "],
        ["Indian", 980, "Bombay Table "],
        ["Chicken", 960, "Roast House "],
        ["French", 940, "Bistro Nord "],
        ["Jewish/Kosher", 641, "Midtown Kosher "],
        ["Ice Cream, Gelato, Yogurt, Ices", 658, "Scoop Parlor "],
        ["Delicatessen", 658, "Neighborhood Deli "]
    ];

    var nextRestNum = 40500000;
    for (var c = 0; c < cuisineFillers.length; c += 1) {
        var cuisineName = cuisineFillers[c][0];
        var needed = cuisineFillers[c][1];
        var labelPrefix = cuisineFillers[c][2];
        for (i = 0; i < needed; i += 1) {
            nextRestNum += 1;
            restaurants.push({
                _id: nextOid(),
                address: {
                    building: String(10 + (i % 900)),
                    coord: [-73.99 + ((i % 50) * 0.001), 40.71 + ((i % 50) * 0.001)],
                    street: "Main Avenue",
                    zipcode: padDigits(10001 + (i % 200), 5)
                },
                borough: boroughs[(c + i) % boroughs.length],
                cuisine: cuisineName,
                grades: [{ date: defaultGradeDate, grade: "A", score: 8 + (i % 5) }],
                name: labelPrefix + padDigits(i + 1, 4),
                restaurant_id: String(nextRestNum)
            });
            if (restaurants.length >= 1000) {
                sdb.restaurants.insertMany(restaurants, { ordered: true });
                restaurants = [];
            }
        }
    }
    if (restaurants.length > 0) {
        sdb.restaurants.insertMany(restaurants, { ordered: true });
    }

    // ---- 2. samples.zips (29,353 documents across 51 states/DC) ---------------------------------

    var stateCounts = [
        ["AK", 195], ["AL", 567], ["AR", 578], ["AZ", 270], ["CA", 1516],
        ["CO", 414], ["CT", 263], ["DC", 24], ["DE", 53], ["FL", 804],
        ["GA", 635], ["HI", 80], ["IA", 922], ["ID", 244], ["IL", 1237],
        ["IN", 676], ["KS", 715], ["KY", 809], ["LA", 464], ["MA", 474],
        ["MD", 420], ["ME", 410], ["MI", 876], ["MN", 882], ["MO", 994],
        ["MS", 363], ["MT", 314], ["NC", 705], ["ND", 391], ["NE", 574],
        ["NH", 218], ["NJ", 540], ["NM", 276], ["NV", 104], ["NY", 1595],
        ["OH", 1007], ["OK", 586], ["OR", 384], ["PA", 1458], ["RI", 69],
        ["SC", 350], ["SD", 384], ["TN", 582], ["TX", 1671], ["UT", 205],
        ["VA", 816], ["VT", 243], ["WA", 484], ["WI", 716], ["WV", 656],
        ["WY", 140]
    ];

    var zipsBatch = [];
    var zipCodeCounter = 0;
    for (var s = 0; s < stateCounts.length; s += 1) {
        var st = stateCounts[s][0];
        var stTotal = stateCounts[s][1];
        for (i = 0; i < stTotal; i += 1) {
            zipCodeCounter += 1;
            zipsBatch.push({
                _id: padDigits(zipCodeCounter, 5),
                city: st + " CITY",
                loc: [-73.95 + ((i % 40) * 0.01), 40.72 + ((i % 40) * 0.01)],
                pop: 1000 + ((i * 37) % 25000),
                state: st
            });
            if (zipsBatch.length >= 1000) {
                sdb.zips.insertMany(zipsBatch, { ordered: true });
                zipsBatch = [];
            }
        }
    }
    if (zipsBatch.length > 0) {
        sdb.zips.insertMany(zipsBatch, { ordered: true });
    }

    // ---- 3. samples.books (431 documents) & remaining sample collections ------------------------

    var books = [];
    for (i = 1; i <= 431; i += 1) {
        books.push({
            _id: i,
            title: "Sample Book " + padDigits(i, 3),
            isbn: "1933988" + padDigits(i, 3),
            pageCount: 200 + (i % 350),
            status: (i % 5 === 0) ? "MEAP" : "PUBLISH",
            authors: ["Author " + String((i % 40) + 1)],
            categories: ["Software Engineering"]
        });
    }
    insertBatched(sdb.books, books);

    sdb.companies.insertMany([
        { _id: nextOid(), name: "Wetpaint", category_code: "web", founded_year: 2005 },
        { _id: nextOid(), name: "AdventNet", category_code: "enterprise", founded_year: 1996 },
        { _id: nextOid(), name: "Zoho", category_code: "software", founded_year: 2005 }
    ], { ordered: true });

    sdb.countries.insertMany([
        { _id: nextOid(), code: "US", name: "United States" },
        { _id: nextOid(), code: "TR", name: "Turkey" },
        { _id: nextOid(), code: "DE", name: "Germany" }
    ], { ordered: true });

    sdb.grades.insertMany([
        { _id: nextOid(), student_id: 0, class_id: 1, scores: [{ type: "exam", score: 88.5 }] },
        { _id: nextOid(), student_id: 1, class_id: 1, scores: [{ type: "exam", score: 92.0 }] },
        { _id: nextOid(), student_id: 2, class_id: 2, scores: [{ type: "exam", score: 79.5 }] }
    ], { ordered: true });

    sdb.inspections.insertMany([
        { _id: nextOid(), id: "10021-2015-ENFO", business_name: "ACME SUPPLY", result: "Pass" },
        { _id: nextOid(), id: "10022-2015-ENFO", business_name: "METRO BAKERY", result: "Pass" },
        { _id: nextOid(), id: "10023-2015-ENFO", business_name: "CITY MARKET", result: "No Violation Issued" }
    ], { ordered: true });
})();
