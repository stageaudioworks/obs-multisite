# Multisite: volunteer card

Print this and keep it beside the OBS machine. The technical lead's version is
[FIRST-SERVICE.md](FIRST-SERVICE.md).

Technical contact: ______________________  Phone: ______________________

---

## Main site (sending)

**Before the service, at least 15 minutes early**

1. Open OBS. Find the **Multisite Encoder** dock (View → Docks if it's hidden).
2. Check **Internet** is green.
3. Press **Go live**. Leave it running.

**During the service:** nothing to do. Press a cue button at the big moments
(Worship, Sermon, Response) if you've been asked to.

**After the service:** press **End broadcast**. It's fine if it says
segments are still uploading — leave the computer on until it says it's done.

| You see | It means | Do |
| --- | --- | --- |
| *no connection — saving to this computer, nothing lost* | The internet dropped. Nothing is lost. | Nothing. Call the contact if it lasts over 5 minutes. |
| *Interrupted event found* when you press Go live | An old broadcast was never ended. | Today's service: press **Start a new event**. If OBS crashed in the last few minutes: **Resume it**. |
| *Resumed event from …* | OBS restarted and carried on the same broadcast. Good. | Nothing. |
| OBS crashed or closed | | Reopen OBS, press **Go live**. It carries on. |

---

## Campus (receiving)

**Before the service**

1. Open OBS. Find the **Multisite Decoder** dock.
2. Check **Internet** is green.
3. Press **Follow live**. Wait while the buffer fills (the dock counts it up).
4. Press **Play**, then **Lock**.

**During the service:** watch **Could broadcast for**. That's how long the
picture keeps going if the internet drops right now.

| You see | It means | Do |
| --- | --- | --- |
| **Internet** red, picture still playing | The internet dropped; you're playing from the reserve. | Nothing yet. If *Could broadcast for* falls under 1 minute, get the fallback ready. |
| Picture frozen or black | | Switch to the **fallback scene**. Then call the contact. |
| *Nothing on air at the main site* | The main site isn't broadcasting. | Call the main site. |
| *offline* | The main site stopped more than 10 minutes ago. | Call the main site. |
| OBS crashed or closed | | Reopen OBS, **Follow live**, wait for the buffer, **Play**. Use the fallback scene meanwhile. |

**Fallback scene:** ______________________ (one click in OBS Scenes)

---

**Afterwards, good day or bad:** Help → Log Files → Upload Current Log File in
OBS, and send the link to the technical contact.
