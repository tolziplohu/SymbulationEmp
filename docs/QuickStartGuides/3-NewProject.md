# Starting a New Project
If you are planning to use Symbulation for a research project, we recommend that you create a new repository for your project. 
One of our goals with this repository is always that it will become useful supplemental material for the paper resulting from the project, and so we will include tips for reaching that goal in this guide.
This is the workflow that we have found easiest to manage while staying organized and keeping everything under version control.

First, make a new git repository, for example [using GitHub](https://docs.github.com/en/get-started/quickstart/create-a-repo).

In that repository, add Symbulation and Empirical as [submodules](https://git-scm.com/book/en/v2/Git-Tools-Submodules):

```
git submodule add https://github.com/anyaevostinar/SymbulationEmp
git submodule add https://github.com/devosoft/Empirical.git
```

Initializing a git repository with submodules is slightly more complicated than normal, so we recommend also including the instructions for doing so in your README:

```
This repository has submodules necessary to rerun the experiments. To fully clone this repository, run:

git clone --recurse-submodules <YOUR REPO ADDRESS HERE>

Or:

git clone <YOUR REPO ADDRESS HERE>
git submodule update --init --recursive
```