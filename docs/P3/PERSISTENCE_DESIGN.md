# Persistence Design


## Requirement


Preserve minimum diagnostic information.


## Options


## Memory Only


Advantages:

- simplest
- no flash write


Disadvantage:

lost after reboot



## Local File


Advantages:

- keep last error


Disadvantage:

flash write


## Database


Not allowed.


Reason:

complexity unnecessary.


## Decision


Use:

Memory runtime state


Optional:

small crash snapshot


Store:


- last state
- last error reason
- timestamp


Do not store:

historical database