# Escape From Monster Manor - Modernised!

A native, faithful source port of the 3DO game EFMM to modern systems.

<img width="1281" height="992" alt="image" src="https://github.com/user-attachments/assets/82e9a8b0-819a-4ccc-bd8f-c3498043cad9" />

It uses SDL3 for completely cross platform video, input and sound systems.

For dealing with the 3DO'isms of the code, I use my compatibility layers I developed for my Doom3DO native port.  As usual, we have our own CCB struct, we fully render and display the CEL images, data is Big Endian etc..  essentially the code thinks its on a 3DO.  I also rewrote the ARM assembler files into portable ANSI C.

The one improvement I made is pixel perfect mouse movement.  Original turning speed / logic is retained for keyboard only and gamepad.

Enjoy!

~Gibbon
