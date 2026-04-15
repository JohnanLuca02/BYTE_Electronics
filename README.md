# BYTE_Electronics
Repository for everything concerning the electronics (PCB, Electrical schemes, BOM, ...) of the robot dog project of the AiTeam called BYTE.

**READ BEFORE STARTING TO WORK**

**Github workflow:**

*ALWAYS* work on your own branch
1) Check that you're in the correct branch:
   - git branch --all
       - if your branch is highlated in *green*, you can go to step 2)
       - if your branch is highlated in *red*, follow the next steps:
           - git fetch --all
           - git checkout [*name of your branch*]
     
2) Once you're in the right branch, *always* start pulling the changes:
   - git pull 
3) Often save your progress:
   - git add .
   - git commit -m "[*Message describing the changes made*]"
4) At the end of your session, ore once you're happy with your work, push your progress on the repo:
   - git push
  
**FOR EASYEDA2KICAD PLUG-IN**

easyeda2kicad --full --lcsc_id [LCSC CODE] --output .
