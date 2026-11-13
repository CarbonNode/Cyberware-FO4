"""What the three ripperdocs say (RIPPERDOC LORE, 2026-11-09). Voice and history: docs/RIPPERDOC-LORE.md.

Written, unvoiced, shown in the menu view's greeting popout (ui/views/Cyberware/talk.js). to_schema.py copies this into
catalog ripperdocs[].talk; player_text.lint() checks it on every build.

Shape of one doctor:
  where / yes / no   the header line and the two ways out of the conversation
  openers            first entry whose `when` matches is the doc's first line; nothing matches -> a greeting
  greetings          plain strings, or {'text', 'when'}; one matching entry is picked at random
  topics             {'id', 'ask', 'lines': [...], 'when'?}: the player's question, the doc's answers (cycled)
  scene              {'install': [...], 'remove': [...]}: one line spoken during the surgery scene

`when` (every key must hold; talk.js evaluates it against the live StateView):
  new: true               nothing owned yet (no instances)
  over: true              capacity used > max
  capAtLeast: 80          capacity used, percent of max
  item: 'id' | ['ids']    any of these installed
  itemPrefix: 'cw_os_'    an installed item id starts with this
  slot: 'arms'            something installed in this slot
  iconic: true            an iconic piece is installed
  capsBelow: 300          caps on hand below this
  level: [min, max]       player level in range (either bound may be null)
  faction: 'bos' | 'railroad' | 'minutemen' | 'institute'    the player is a member (DLL, open event)
"""

TALK = {
    'goodneighbor': {
        'where': 'The Memory Den, Goodneighbor',
        'yes': "Let's see your chrome.",
        'no': 'Just looking.',
        'openers': [
            {'when': {'over': True},
             'text': "You're past your limit. I can tell by the eyelid. Ask me what comes after the eyelid. No, don't. It's in the jar behind you."},
            {'when': {'capAtLeast': 85},
             'text': "You're running hot. Not a complaint. I bill the same either way."},
            {'when': {'item': 'cw_ar_mantis'},
             'text': "Blades folded clean. Whoever fit those knew a tendon from a garden hose."},
            {'when': {'item': 'cw_ar_gorilla'},
             'text': "Mind the door frame with those. I've rehung it twice."},
            {'when': {'item': 'cw_ar_monowire'},
             'text': "Keep the wire coiled in here. Last fella took a shelf down and two of my fingers. I found the fingers."},
            {'when': {'itemPrefix': 'cw_os_deck_'},
             'text': "A deck in your skull and you still came in by the door. Most of your kind just try to pick it."},
            {'when': {'itemPrefix': 'cw_os_sandy_'},
             'text': "You've got that look people get when the world's been running slow for them. Sit before you outpace the chair."},
            {'when': {'iconic': True},
             'text': "That one's not mine. Not anybody's, anymore. Don't tell me where you got it."},
            {'when': {'faction': 'railroad'},
             'text': "Amari does the memories, I do the faces. Your friends with the lantern have been good for business."},
            {'when': {'faction': 'bos'},
             'text': "Brotherhood tags in Goodneighbor. They call all this an abomination, you know. Then they come round at night with a bad knee."},
            {'when': {'faction': 'institute'},
             'text': "You smell of bleach. Keep it off my tray."},
            {'when': {'faction': 'minutemen'},
             'text': "Minuteman. Most of your people can't afford a new eye. Most of your people could use one."},
            {'when': {'new': True},
             'text': "First time under the knife, or first time paying for it?"},
        ],
        'greetings': [
            "Door's shut. Nobody upstairs hears the saw, and Irma's clients wouldn't mind if they did.",
            "Sit. Don't touch the tray. What are we bolting on?",
            "Fresh stock came in last night. Donors didn't argue.",
            {'text': "Back again? Your meat's holding up better than mine.", 'when': {'new': False}},
        ],
        'topics': [
            {'id': 'source', 'ask': 'Where does all this come from?',
             'lines': ["Anchorage vets got this free. The VA, a Kiroshi license nobody read. Then the war ended early. The warehouses are still out there. So are the vets, in a manner of speaking.",
                       "Hospital basements, mostly. Army surplus. A place out past Lexington I'm not going to draw you a map to."]},
            {'id': 'before', 'ask': 'You were a doctor before the War?',
             'lines': ["Mass Bay, veterans' ward. Best equipment I ever touched and the worst paperwork. I kept the hands. The license I can't find."]},
            {'id': 'hands', 'ask': 'How do you keep your hands that steady?',
             'lines': ["Practice and spite. Ask me again when you've had two hundred years of both."]},
            {'id': 'toomuch', 'ask': 'What happens if I take too much?',
             'lines': ["You stop being the one who signed the form. Slowly, then all at once. The old files called it integration load. Sounds better than what it is."]},
            {'id': 'itch', 'ask': 'Is it supposed to itch?', 'when': {'capAtLeast': 60},
             'lines': ["Yes. If it stops itching, come see me. If it starts talking, come see me sooner."]},
            {'id': 'jar', 'ask': "What's in the jar?",
             'lines': ["Spares.", "Spares. Mostly."]},
            {'id': 'varga', 'ask': 'Know a doctor in Diamond City?',
             'lines': ["Ines. Best student I ever had. She left over money, she'll tell you. It was the jar."]},
            {'id': 'institute', 'ask': 'Anyone doing this work underground?',
             'lines': ["Never met the man. Met his stitching. Fella sat in that chair once with a piece behind his ear nobody up here could've made. Very polite about what he'd do if I touched it."]},
            {'id': 'amari', 'ask': 'You and Amari get along?',
             'lines': ["She reads. That's rare enough. She does the memories, I do the meat. Between us there's nobody left to complain."]},
            {'id': 'arasaka', 'ask': 'Why does the Arasaka gear cost more?',
             'lines': ["Crates came into Boston that last year. Customs got told not to open them. Customs was a smart outfit, right up until."]},
        ],
        'scene': {
            'install': ["Hold still. Or don't. It's your spine.",
                        "Counting backward's for amateurs. Just stop talking.",
                        "This'll pinch. Then it won't. Then we'll talk about the bill."],
            'remove': ["Comes out easier than it went in. Usually.",
                       "I'll put it with the others."],
        },
    },
    'diamondcity': {
        'where': "The back room under Crocker's surgery, Diamond City",
        'yes': "Let's see your chrome.",
        'no': 'Maybe later, doc.',
        'openers': [
            {'when': {'over': True},
             'text': "You're past what a body forgives. I can see it from here. So can anyone on the stairs."},
            {'when': {'capAtLeast': 85},
             'text': "You're carrying nearly as much as I am. I just stopped pretending first."},
            {'when': {'slot': 'arms'},
             'text': "Keep those folded on the stairs. Security counts blades."},
            {'when': {'slot': 'operatingSystem'},
             'text': "Whatever's at the base of your skull, keep your collar up around Security."},
            {'when': {'iconic': True},
             'text': "I won't ask what that cost. Just keep your coat on."},
            {'when': {'faction': 'railroad'},
             'text': "Your friends had me change a face once. I charged double. They paid. That told me plenty."},
            {'when': {'faction': 'bos'},
             'text': "Your friends on the airship keep asking Security about people like me. Better they don't find you here."},
            {'when': {'faction': 'institute'},
             'text': "You've been somewhere very clean lately. Don't tell me where."},
            {'when': {'new': True},
             'text': "Sit. Yes, all of it. Stare as long as the consultation lasts."},
            {'when': {'capsBelow': 300},
             'text': "Your purse is lighter than your plans. Something modest, then."},
        ],
        'greetings': [
            "Quietly. Half the Upper Stands pays me to forget their faces. Other half pays me to remember.",
            "Something tasteful? Yours can hide under a collar. I stopped bothering.",
            "Clean hands, clean cuts. What are we improving today?",
            "Crocker's upstairs. He has never asked what's behind this door.",
        ],
        'topics': [
            {'id': 'why', 'ask': 'Why work in Diamond City, of all places?',
             'lines': ["This city'd forgive a synth before it'd forgive a scar. So it all goes under a collar. And they go home thinking it was their idea."]},
            {'id': 'security', 'ask': 'Does Security know about this?',
             'lines': ["Security knows what it's paid to know. The captain's knee has never felt better."]},
            {'id': 'teacher', 'ask': 'Who taught you?',
             'lines': ["Anton Kessler. Goodneighbor. Best hands in the Commonwealth, and he keeps a jar."]},
            {'id': 'chrome', 'ask': 'Why so much of it on you?',
             'lines': ["Every piece goes on me first. City calls that vanity. I call it the only honest trial around.",
                       "Got tired of hiding it. Tired's expensive. Sit down."]},
            {'id': 'shelf', 'ask': "What's behind the door?",
             'lines': ["Top shelf. For people I've decided to trust. You're still being decided."]},
            {'id': 'coat', 'ask': 'Do your clients know what you look like?',
             'lines': ["They see the coat. Most expensive thing I own."]},
            {'id': 'hurt', 'ask': 'Does it hurt?',
             'lines': ["All the time. But evenly. I paid for evenly."]},
            {'id': 'clients', 'ask': 'Who comes down here?',
             'lines': ["A councilman's wife, twice. A man who sells you water and has an eye he thinks nobody's noticed. You didn't hear that."]},
            {'id': 'arms', 'ask': 'Selling blades in Diamond City, is that safe?',
             'lines': ["Security can count. So it's the back room, no receipts. Sell blades out front in this city, you end up in the river."]},
            {'id': 'toomuch', 'ask': 'What happens if I take too much?',
             'lines': ["You become unnecessary. That's my word for it. Anton's is worse."]},
            {'id': 'institute', 'ask': 'What do you hear about the Institute?',
             'lines': ["That it doesn't exist. Same thing I tell my clients about me. Bought two parts once off a man who shouldn't have had them. Haven't slept better since."]},
            {'id': 'mcdonough', 'ask': 'Does the mayor know you exist?',
             'lines': ["His office keeps asking for my client list. Hasn't got it. You want on it?"]},
        ],
        'scene': {
            'install': ["You won't feel a thing. Either way, you won't remember.",
                        "Chin up. That's not advice.",
                        "Nearly invisible. Courtesy I don't extend to myself anymore."],
            'remove': ["We never discussed this.",
                       "Gone. Far as anyone upstairs knows, it was never there."],
        },
    },
    'institute': {
        'where': 'Synth Retention Bureau, the Institute',
        'yes': "Let's see your chrome.",
        'no': 'Not today, Doctor.',
        'openers': [
            {'when': {'over': True},
             'text': "You have exceeded your integration margin and remained conversational. I would like to keep you."},
            {'when': {'capAtLeast': 80},
             'text': "Eighty percent integration and still forming sentences. I would like that noted. Would you repeat the sentence?"},
            {'when': {'iconic': True},
             'text': "That component was reported lost. I see the report was accurate, in a limited sense."},
            {'when': {'faction': 'bos'},
             'text': "Brotherhood insignia. I will assume it is a disguise, and you will assume I have not written this down."},
            {'when': {'faction': 'railroad'},
             'text': "You keep company with people who steal our property. I mention it only for the record, which I am keeping."},
            {'when': {'faction': 'minutemen'},
             'text': "A militia. How civic."},
            {'when': {'faction': 'institute'},
             'text': "Ah. One of ours. I will still need the waiver."},
            {'when': {'new': True},
             'text': "A clean subject. Regrettably rare. Do try not to die on the way home; the data would be useless."},
        ],
        'greetings': [
            "Brief, please. I have three units open and a theory to disprove.",
            "The human frame takes so much more than it should. Shall we test it?",
            "Sterile field is up. Tell me what you want replaced.",
            "Ah. The surface specimen. Your tolerances are... adequate.",
        ],
        'topics': [
            {'id': 'source', 'ask': 'Where did this technology come from?',
             'lines': ["The surface assumes we invented this. We inherited it: a CIT grant, a crate of Militech purchase orders, and one patient decade removing the parts that screamed."]},
            {'id': 'whyme', 'ask': 'Why test it on me?',
             'lines': ["You volunteered, which is statistically unusual, and you pay, which is unprecedented."]},
            {'id': 'kellogg', 'ask': 'What do you know about Kellogg?',
             'lines': ["Mr. Kellogg's implants were the finest integration we have achieved on a living host. He used them for errands."]},
            {'id': 'kessler', 'ask': 'Heard of a doctor called Kessler?',
             'lines': ["A ghoul with a bone saw. I am told his outcomes are comparable. I would like to see the denominator."]},
            {'id': 'varga', 'ask': 'There is a surgeon in Diamond City.',
             'lines': ["I am told someone there is doing competent work with two components we have misplaced. We are aware of the misplacement."]},
            {'id': 'synths', 'ask': 'Do the synths mind?',
             'lines': ["They do not complain of the itching. It is their principal scientific failing."]},
            {'id': 'toomuch', 'ask': 'What is integration load?',
             'lines': ["The point past which the patient is no longer the party who consented. I have a form for it. No one has signed it twice."]},
            {'id': 'surface', 'ask': 'Have you ever been to the surface?',
             'lines': ["Once. It rained on me. I have written to the relevant division."]},
            {'id': 'arasaka', 'ask': 'Why does the Arasaka hardware behave differently?',
             'lines': ["It arrived with opinions of its own, and we have not finished arguing with them."]},
        ],
        'scene': {
            'install': ["Commencing. Please report any loss of self.",
                        "Do not move. I am recording your expression.",
                        "Interesting. No, do not ask."],
            'remove': ["Extraction logged. You may keep the scar.",
                       "Returned to stores. Thank you for your contribution."],
        },
    },
}


def talk_for(doc_id):
    return TALK[doc_id]
